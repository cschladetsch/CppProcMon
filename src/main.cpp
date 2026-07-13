#include "GpuMonitor.h"
#include "ProcessMonitor.h"
#include "SystemInfo.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// WIN32_LEAN_AND_MEAN comes from the procmon target's compile definitions
// (ProcMon.lm -> generated/CMakeLists.txt), not a local #define.
#include <windows.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Small formatting helpers
// ---------------------------------------------------------------------------

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(len > 0 ? len - 1 : 0), L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
    return out;
}

std::string FormatBytes(uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return buf;
}

// ---------------------------------------------------------------------------
// Shared state between the polling worker thread and the render thread.
// ---------------------------------------------------------------------------

struct SharedState {
    std::mutex mutex;
    std::vector<ProcessSample> samples;
    SystemMemoryInfo memInfo;
    std::vector<GpuAdapterInfo> gpuAdapters;
    double lastPollDurationMs = 0.0;
};

std::atomic<bool> g_running{true};
std::atomic<float> g_intervalSeconds{1.0f};

void WorkerThread(SharedState* state) {
    ProcessMonitor procMon;
    GpuMonitor gpuMon;

    {
        auto adapters = GetGpuAdapters();
        std::lock_guard<std::mutex> lock(state->mutex);
        state->gpuAdapters = std::move(adapters);
    }

    auto prevTick = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        auto start = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(start - prevTick).count();
        prevTick = start;

        auto samples = procMon.Poll(elapsed);
        auto gpuStats = gpuMon.Poll();

        for (auto& s : samples) {
            auto it = gpuStats.find(s.pid);
            if (it != gpuStats.end()) {
                s.gpuDedicatedBytes = it->second.dedicatedBytes;
                s.gpuSharedBytes = it->second.sharedBytes;
                s.gpuPercent = it->second.utilizationPercent;
            }
        }

        auto memInfo = GetSystemMemoryInfo();
        auto end = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->samples = std::move(samples);
            state->memInfo = memInfo;
            state->lastPollDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
        }

        float interval = g_intervalSeconds.load(std::memory_order_relaxed);
        if (interval < 0.1f) interval = 0.1f;
        auto target = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                   std::chrono::duration<double>(interval));
        while (g_running.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < target) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

void GlfwErrorCallback(int error, const char* description) {
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// Opens a fresh handle with just PROCESS_TERMINATE (the read-only polling
// handles in ProcessMonitor deliberately don't carry this right) and kills
// the process. Returns false + fills outError on failure (e.g. access
// denied for elevated/protected processes when we're not running as admin).
bool KillProcessById(uint32_t pid, DWORD& outError) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) {
        outError = GetLastError();
        return false;
    }
    BOOL ok = TerminateProcess(h, 1);
    if (!ok) {
        outError = GetLastError();
    }
    CloseHandle(h);
    return ok != 0;
}

enum class SortColumn { Pid, Name, Cpu, Ram, Private, VramDedicated, VramShared, Gpu };

// ---------------------------------------------------------------------------
// CPU-over-time graph view.
// ---------------------------------------------------------------------------

// Longest time window the "Time window" slider allows (300s = 5 min). History
// older than this is pruned every frame regardless of the slider's current
// setting, so widening the slider never has to wait for data to accumulate.
constexpr double kMaxHistorySeconds = 300.0;

struct HistorySample {
    double t = 0.0;       // seconds since app start
    float cpuPercent = 0.0f;
};

struct ProcHistory {
    std::wstring name;
    std::deque<HistorySample> samples;
};

// Stable, visually-distinct color per PID: hash the PID into a hue so a
// given process keeps the same color across frames (and, usually, across
// runs) without needing a palette that can run out of entries.
ImU32 ColorForPid(uint32_t pid) {
    uint32_t h = pid * 2654435761u; // Knuth multiplicative hash
    float hue = static_cast<float>(h % 360u) / 360.0f;
    ImVec4 c = static_cast<ImVec4>(ImColor::HSV(hue, 0.65f, 0.95f));
    return ImGui::ColorConvertFloat4ToU32(c);
}

} // namespace

int main() {
    SharedState state;
    std::thread worker(WorkerThread, &state);

    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) {
        g_running.store(false);
        worker.join();
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "ProcMon - CPU / RAM / VRAM Monitor", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        g_running.store(false);
        worker.join();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    char filterBuf[128] = "";
    float intervalUi = g_intervalSeconds.load(std::memory_order_relaxed);

    std::vector<ProcessSample> displaySamples;

    // CPU-over-time graph view: on by default per-process, replacing the
    // table with a line graph (X = time, Y = CPU%) for whichever processes
    // are, together, responsible for the top 75% of currently-tracked CPU
    // usage — the "Time window" slider controls how much history is shown.
    bool graphView = true;
    float graphWindowSeconds = 60.0f;
    std::unordered_map<uint32_t, ProcHistory> history;
    const auto appStart = std::chrono::steady_clock::now();

    // Set by double-clicking a row in list view: pins that process into the
    // graph (in addition to whatever clears the 75% cumulative bar) so you
    // can see a specific process "in relation to" the top offenders, even
    // if it wouldn't normally make the cut on its own.
    uint32_t focusedPid = 0;

    // Right-click "Kill Process" state, shared across frames.
    uint32_t pendingKillPid = 0;
    std::wstring pendingKillName;
    std::string statusMessage;

    while (!glfwWindowShouldClose(window)) {
        // glfwPollEvents() returns immediately, so a plain poll loop redraws
        // at whatever rate vsync allows (typically 60Hz) forever, even while
        // completely idle between polls - wasted CPU/GPU for a monitoring
        // tool that only actually has new data once per intervalSeconds.
        // glfwWaitEventsTimeout blocks until either a real input event (so
        // dragging sliders/typing stays fully responsive - each mouse move
        // or keystroke is its own event that wakes it immediately) or the
        // timeout elapses, so steady-state redraws drop to roughly once per
        // update interval instead of once per frame.
        glfwWaitEventsTimeout(static_cast<double>(std::max(0.05f, intervalUi)));

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        SystemMemoryInfo memInfo;
        std::vector<GpuAdapterInfo> adapters;
        double pollMs = 0.0;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            displaySamples = state.samples;
            memInfo = state.memInfo;
            adapters = state.gpuAdapters;
            pollMs = state.lastPollDurationMs;
        }

        // Append this frame's samples to per-PID history and prune anything
        // older than the longest window the slider allows. Done every frame
        // regardless of graphView so the graph has a full window of data
        // the moment it's toggled on, instead of building up from empty.
        {
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - appStart).count();
            for (const auto& s : displaySamples) {
                ProcHistory& h = history[s.pid];
                h.name = s.name;
                h.samples.push_back(HistorySample{now, static_cast<float>(s.cpuPercent)});
                while (!h.samples.empty() && h.samples.front().t < now - kMaxHistorySeconds) {
                    h.samples.pop_front();
                }
            }
            // Drop history for processes that are gone AND have aged fully
            // out of the max window, so churny process lists don't leak
            // memory forever.
            for (auto it = history.begin(); it != history.end();) {
                if (!it->second.samples.empty() && it->second.samples.back().t < now - kMaxHistorySeconds) {
                    it = history.erase(it);
                } else {
                    ++it;
                }
            }
        }

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("ProcMon", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoBringToFrontOnFocus);

        double totalCpu = 0.0;
        uint64_t totalDedicatedVram = 0;
        for (auto& s : displaySamples) {
            totalCpu += s.cpuPercent;
            totalDedicatedVram += s.gpuDedicatedBytes;
        }

        ImGui::Text("CPU: %.1f%%   RAM: %s / %s   (last poll: %.1f ms)", totalCpu,
                    FormatBytes(memInfo.totalPhysicalBytes - memInfo.availablePhysicalBytes).c_str(),
                    FormatBytes(memInfo.totalPhysicalBytes).c_str(), pollMs);

        if (!adapters.empty()) {
            uint64_t totalAdapterVram = 0;
            for (auto& a : adapters) totalAdapterVram += a.dedicatedVideoMemoryBytes;
            std::string gpuLine = "VRAM: " + FormatBytes(totalDedicatedVram) + " / " +
                                   FormatBytes(totalAdapterVram) + "  (" + WideToUtf8(adapters[0].description) + ")";
            ImGui::TextUnformatted(gpuLine.c_str());
        } else {
            ImGui::TextUnformatted("VRAM: no hardware adapter detected / GPU counters unavailable");
        }

        if (!statusMessage.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", statusMessage.c_str());
        }

        ImGui::Separator();

        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##filter", "Filter by process name...", filterBuf, sizeof(filterBuf));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Update interval (s)", &intervalUi, 0.25f, 10.0f, "%.2f")) {
            g_intervalSeconds.store(intervalUi, std::memory_order_relaxed);
        }

        ImGui::Checkbox("Graph view", &graphView);
        if (graphView) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220);
            ImGui::SliderFloat("Time window (s)", &graphWindowSeconds, 5.0f, static_cast<float>(kMaxHistorySeconds),
                                "%.0f");
        }

        ImGui::Separator();

        if (!graphView) {
        ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                                      ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

        if (ImGui::BeginTable("processes", 8, tableFlags, ImVec2(0, 0))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70.0f, (ImGuiID)SortColumn::Pid);
            ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch, 0.0f, (ImGuiID)SortColumn::Name);
            ImGui::TableSetupColumn("CPU %",
                                     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                         ImGuiTableColumnFlags_PreferSortDescending,
                                     80.0f, (ImGuiID)SortColumn::Cpu);
            ImGui::TableSetupColumn("RAM (WS)",
                                     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                                     100.0f, (ImGuiID)SortColumn::Ram);
            ImGui::TableSetupColumn("RAM (Private)",
                                     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                                     110.0f, (ImGuiID)SortColumn::Private);
            ImGui::TableSetupColumn("VRAM (Dedicated)",
                                     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                                     130.0f, (ImGuiID)SortColumn::VramDedicated);
            ImGui::TableSetupColumn("VRAM (Shared)",
                                     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                                     120.0f, (ImGuiID)SortColumn::VramShared);
            ImGui::TableSetupColumn("GPU %",
                                     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                                     80.0f, (ImGuiID)SortColumn::Gpu);
            ImGui::TableHeadersRow();

            std::wstring filterW = Utf8ToWide(filterBuf);
            for (auto& c : filterW) c = static_cast<wchar_t>(towlower(c));

            std::vector<ProcessSample*> rows;
            rows.reserve(displaySamples.size());
            for (auto& s : displaySamples) {
                if (!filterW.empty()) {
                    std::wstring nameLower = s.name;
                    for (auto& c : nameLower) c = static_cast<wchar_t>(towlower(c));
                    if (nameLower.find(filterW) == std::wstring::npos) continue;
                }
                rows.push_back(&s);
            }

            if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
                if (sortSpecs->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                    SortColumn col = static_cast<SortColumn>(spec.ColumnUserID);
                    bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;

                    std::sort(rows.begin(), rows.end(), [&](const ProcessSample* a, const ProcessSample* b) {
                        int cmp = 0;
                        switch (col) {
                            case SortColumn::Pid:
                                cmp = (a->pid < b->pid) ? -1 : (a->pid > b->pid ? 1 : 0);
                                break;
                            case SortColumn::Name:
                                cmp = _wcsicmp(a->name.c_str(), b->name.c_str());
                                break;
                            case SortColumn::Cpu:
                                cmp = (a->cpuPercent < b->cpuPercent) ? -1 : (a->cpuPercent > b->cpuPercent ? 1 : 0);
                                break;
                            case SortColumn::Ram:
                                cmp = (a->workingSetBytes < b->workingSetBytes)
                                          ? -1
                                          : (a->workingSetBytes > b->workingSetBytes ? 1 : 0);
                                break;
                            case SortColumn::Private:
                                cmp = (a->privateBytes < b->privateBytes) ? -1
                                                                           : (a->privateBytes > b->privateBytes ? 1 : 0);
                                break;
                            case SortColumn::VramDedicated:
                                cmp = (a->gpuDedicatedBytes < b->gpuDedicatedBytes)
                                          ? -1
                                          : (a->gpuDedicatedBytes > b->gpuDedicatedBytes ? 1 : 0);
                                break;
                            case SortColumn::VramShared:
                                cmp = (a->gpuSharedBytes < b->gpuSharedBytes)
                                          ? -1
                                          : (a->gpuSharedBytes > b->gpuSharedBytes ? 1 : 0);
                                break;
                            case SortColumn::Gpu:
                                cmp = (a->gpuPercent < b->gpuPercent) ? -1 : (a->gpuPercent > b->gpuPercent ? 1 : 0);
                                break;
                        }
                        return ascending ? (cmp < 0) : (cmp > 0);
                    });
                }
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const ProcessSample* s = rows[i];
                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(s->pid));

                    // Invisible selectable spanning the whole row: gives us a
                    // right-click target that covers every column, not just
                    // whichever cell happens to be under the cursor. Also
                    // doubles as the double-click target that jumps to graph
                    // view focused on this process.
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable("##rowhit", false,
                                           ImGuiSelectableFlags_SpanAllColumns |
                                               ImGuiSelectableFlags_AllowOverlap |
                                               ImGuiSelectableFlags_AllowDoubleClick) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        graphView = true;
                        focusedPid = s->pid;
                    }

                    if (ImGui::BeginPopupContextItem("row_context")) {
                        ImGui::TextUnformatted(WideToUtf8(s->name).c_str());
                        ImGui::Text("PID: %u", s->pid);
                        if (!s->imagePath.empty()) {
                            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                            ImGui::TextUnformatted(WideToUtf8(s->imagePath).c_str());
                            ImGui::PopTextWrapPos();
                        } else {
                            ImGui::TextDisabled("(path unavailable - process may be protected)");
                        }
                        ImGui::Separator();
                        ImGui::Text("CPU:              %.1f%%", s->cpuPercent);
                        ImGui::Text("RAM (working set): %s", FormatBytes(s->workingSetBytes).c_str());
                        ImGui::Text("RAM (private):     %s", FormatBytes(s->privateBytes).c_str());
                        ImGui::Text("VRAM (dedicated):  %s", FormatBytes(s->gpuDedicatedBytes).c_str());
                        ImGui::Text("VRAM (shared):     %s", FormatBytes(s->gpuSharedBytes).c_str());
                        ImGui::Text("GPU:              %.1f%%", s->gpuPercent);
                        ImGui::Separator();
                        if (ImGui::MenuItem("Copy PID")) {
                            ImGui::SetClipboardText(std::to_string(s->pid).c_str());
                        }
                        if (ImGui::MenuItem("Copy image path", nullptr, false, !s->imagePath.empty())) {
                            ImGui::SetClipboardText(WideToUtf8(s->imagePath).c_str());
                        }
                        ImGui::Separator();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
                        if (ImGui::MenuItem("Kill Process")) {
                            pendingKillPid = s->pid;
                            pendingKillName = s->name;
                            ImGui::CloseCurrentPopup();
                            ImGui::OpenPopup("Confirm Kill Process");
                        }
                        ImGui::PopStyleColor();
                        ImGui::EndPopup();
                    }

                    ImGui::SameLine();
                    ImGui::Text("%u", s->pid);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(WideToUtf8(s->name).c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.1f", s->cpuPercent);

                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(FormatBytes(s->workingSetBytes).c_str());

                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(FormatBytes(s->privateBytes).c_str());

                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(FormatBytes(s->gpuDedicatedBytes).c_str());

                    ImGui::TableSetColumnIndex(6);
                    ImGui::TextUnformatted(FormatBytes(s->gpuSharedBytes).c_str());

                    ImGui::TableSetColumnIndex(7);
                    ImGui::Text("%.1f", s->gpuPercent);

                    ImGui::PopID();
                }
            }

            ImGui::EndTable();
        }
        } else {
            // Graph view: X = time (last graphWindowSeconds), Y = CPU%.
            // Only the processes that, sorted by current CPU% descending,
            // are needed to push the cumulative total over 75% get drawn -
            // this keeps the graph readable by dropping the long tail of
            // near-idle processes. Same name filter as the table applies.
            std::wstring filterW = Utf8ToWide(filterBuf);
            for (auto& c : filterW) c = static_cast<wchar_t>(towlower(c));

            std::vector<const ProcessSample*> candidates;
            candidates.reserve(displaySamples.size());
            for (auto& s : displaySamples) {
                if (!filterW.empty()) {
                    std::wstring nameLower = s.name;
                    for (auto& c : nameLower) c = static_cast<wchar_t>(towlower(c));
                    if (nameLower.find(filterW) == std::wstring::npos) continue;
                }
                candidates.push_back(&s);
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const ProcessSample* a, const ProcessSample* b) { return a->cpuPercent > b->cpuPercent; });

            double totalForSelection = 0.0;
            for (auto* s : candidates) totalForSelection += s->cpuPercent;

            std::vector<const ProcessSample*> selected;
            double cumulative = 0.0;
            for (auto* s : candidates) {
                if (!selected.empty() && cumulative >= totalForSelection * 0.75) break;
                selected.push_back(s);
                cumulative += s->cpuPercent;
            }

            // A double-clicked process from list view stays pinned into the
            // graph even if it doesn't crack the top-75% cut, so it can be
            // seen "in relation to" the processes that do.
            if (focusedPid != 0) {
                bool alreadyIn = std::any_of(selected.begin(), selected.end(),
                                              [&](const ProcessSample* s) { return s->pid == focusedPid; });
                if (!alreadyIn) {
                    auto it = std::find_if(displaySamples.begin(), displaySamples.end(),
                                            [&](const ProcessSample& s) { return s.pid == focusedPid; });
                    if (it != displaySamples.end()) {
                        selected.push_back(&*it);
                    } else {
                        // Process exited or aged out - nothing left to pin.
                        focusedPid = 0;
                    }
                }
            }

            const double nowT = std::chrono::duration<double>(std::chrono::steady_clock::now() - appStart).count();
            const double windowStart = nowT - graphWindowSeconds;

            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 graphSize(avail.x - 220.0f, avail.y - 8.0f);
            if (graphSize.x < 100.0f) graphSize.x = 100.0f;
            if (graphSize.y < 100.0f) graphSize.y = 100.0f;

            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            const ImU32 axisColor = IM_COL32(120, 120, 130, 255);
            const ImU32 gridColor = IM_COL32(60, 60, 68, 255);
            const ImU32 textColor = IM_COL32(200, 200, 205, 255);

            drawList->AddRectFilled(origin, ImVec2(origin.x + graphSize.x, origin.y + graphSize.y),
                                     IM_COL32(18, 18, 22, 255));
            drawList->AddRect(origin, ImVec2(origin.x + graphSize.x, origin.y + graphSize.y), axisColor);

            // Horizontal gridlines + Y axis (usage %) labels.
            for (int p = 0; p <= 100; p += 25) {
                float y = origin.y + graphSize.y * (1.0f - static_cast<float>(p) / 100.0f);
                drawList->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + graphSize.x, y), gridColor);
                char label[8];
                snprintf(label, sizeof(label), "%d%%", p);
                drawList->AddText(ImVec2(origin.x + 4.0f, y - 14.0f), textColor, label);
            }

            // Vertical gridlines + X axis (time, "-Ns" = N seconds ago).
            const int timeTicks = 6;
            for (int i = 0; i <= timeTicks; ++i) {
                float frac = static_cast<float>(i) / timeTicks;
                float x = origin.x + graphSize.x * frac;
                drawList->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + graphSize.y), gridColor);
                double secondsAgo = graphWindowSeconds * (1.0 - frac);
                char label[16];
                snprintf(label, sizeof(label), "-%.0fs", secondsAgo);
                drawList->AddText(ImVec2(x + 2.0f, origin.y + graphSize.y - 16.0f), textColor, label);
            }

            for (auto* s : selected) {
                auto it = history.find(s->pid);
                if (it == history.end() || it->second.samples.size() < 2) continue;

                bool isFocused = (s->pid == focusedPid);
                ImU32 color = ColorForPid(s->pid);
                float lineWidth = isFocused ? 3.5f : 2.0f;
                ImVec2 prev{};
                bool havePrev = false;
                for (const auto& sample : it->second.samples) {
                    if (sample.t < windowStart) continue;
                    float xFrac = static_cast<float>((sample.t - windowStart) / graphWindowSeconds);
                    float yFrac = std::min(sample.cpuPercent, 100.0f) / 100.0f;
                    ImVec2 p(origin.x + graphSize.x * xFrac, origin.y + graphSize.y * (1.0f - yFrac));
                    if (havePrev) {
                        drawList->AddLine(prev, p, color, lineWidth);
                    }
                    prev = p;
                    havePrev = true;
                }
                // Halo the last point of the focused process so it's easy to
                // spot even when several lines cross near it.
                if (isFocused && havePrev) {
                    drawList->AddCircle(prev, 5.0f, color, 0, 2.0f);
                }
            }

            ImGui::Dummy(graphSize);

            // Legend: colored swatch + name + current CPU% for each process
            // actually being graphed.
            ImGui::SameLine();
            ImGui::BeginChild("graph_legend", ImVec2(0, graphSize.y), true);
            ImGui::TextDisabled("Top procs (cumulative CPU > 75%%)");
            if (focusedPid != 0) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear focus")) {
                    focusedPid = 0;
                }
            }
            ImGui::Separator();
            for (auto* s : selected) {
                bool isFocused = (s->pid == focusedPid);
                ImVec4 c = ImGui::ColorConvertU32ToFloat4(ColorForPid(s->pid));
                ImGui::ColorButton("##swatch", c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                                    ImVec2(12, 12));
                ImGui::SameLine();
                if (isFocused) {
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s  %.1f%%  (focused)",
                                        WideToUtf8(s->name).c_str(), s->cpuPercent);
                } else {
                    ImGui::Text("%s  %.1f%%", WideToUtf8(s->name).c_str(), s->cpuPercent);
                }
            }
            if (selected.empty()) {
                ImGui::TextDisabled("(no processes)");
            }
            ImGui::EndChild();
        }

        // Confirmation modal for "Kill Process", opened from the row context
        // menu above. Kept outside the table/clipper so it isn't tied to a
        // specific row's lifetime.
        ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Confirm Kill Process", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Terminate \"%s\" (PID %u)?", WideToUtf8(pendingKillName).c_str(), pendingKillPid);
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "This cannot be undone.");
            ImGui::Spacing();
            if (ImGui::Button("Terminate", ImVec2(120, 0))) {
                DWORD err = 0;
                if (!KillProcessById(pendingKillPid, err)) {
                    statusMessage = "Failed to terminate PID " + std::to_string(pendingKillPid) + " (error " +
                                     std::to_string(err) + ")";
                } else {
                    statusMessage.clear();
                }
                pendingKillPid = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                pendingKillPid = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Text("%d processes", static_cast<int>(displaySamples.size()));

        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    g_running.store(false, std::memory_order_relaxed);
    worker.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
