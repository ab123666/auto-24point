// 轮询帧节拍实测：统计 DXGI AcquireNextFrame 在画面变化期间的帧间隔分布。
// 用法：在游戏发牌/对局动画期间运行 tests\test_poll.exe [秒数，默认8]（双击或任意目录运行均可）
// 输出：成功新帧的间隔 p50/p95/p99/max、等效帧率、静止超时次数；结果同时写入 exe 同目录 poll_result.txt
// 牌面检测延迟下限 ≈ 1个画面更新帧（牌静止后的下一帧）+ 约1ms识别。
#define main disabled_24point_main
#include "../auto_24point.cpp"
#undef main

#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <ctime>

int main(int argc, char** argv) {
    int seconds = (argc > 1) ? max(1, atoi(argv[1])) : 8;
    if (!g_dxgi.init()) { cerr << "DXGI init failed" << endl; return 1; }

    // 结果文件固定写到 exe 所在目录，双击/任意 cwd 运行都能落盘
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    string exeDir = exePath;
    size_t slash = exeDir.find_last_of("\\/");
    string outPath = (slash == string::npos) ? "poll_result.txt"
                                            : exeDir.substr(0, slash) + "\\poll_result.txt";

    LARGE_INTEGER freq, tStart, now, last{0};
    QueryPerformanceFrequency(&freq);
    vector<double> gaps;
    int frames = 0, timeouts = 0, errors = 0;

    cout << "开始采样 " << seconds << " 秒——请让游戏画面保持动画/对局状态..." << endl;
    QueryPerformanceCounter(&tStart);
    for (;;) {
        IDXGIResource* res = nullptr;
        DXGI_OUTDUPL_FRAME_INFO fi;
        HRESULT hr = g_dxgi.duplication->AcquireNextFrame(20, &fi, &res);
        QueryPerformanceCounter(&now);
        double elapsedMs = (double)(now.QuadPart - tStart.QuadPart) * 1000.0 / freq.QuadPart;
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            timeouts++;
        } else if (SUCCEEDED(hr)) {
            if (last.QuadPart) gaps.push_back((double)(now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart);
            last = now;
            frames++;
            res->Release();
            g_dxgi.duplication->ReleaseFrame();
        } else {
            errors++;
            if (hr == DXGI_ERROR_ACCESS_LOST) { cerr << "ACCESS_LOST" << endl; break; }
        }
        if (elapsedMs > seconds * 1000.0) break;
    }
    g_dxgi.release();

    ostringstream rep;
    rep << "\n=== " << seconds << " 秒采样结果 ===" << endl;
    rep << "新帧总数: " << frames << "，静止超时: " << timeouts << "，错误: " << errors << endl;
    if (gaps.empty()) {
        rep << "画面几乎无变化（静止桌面），请在游戏动画期间重测" << endl;
        cout << rep.str();
        ofstream rf(outPath, ios::app);
        rf << rep.str();
        return 0;
    }
    sort(gaps.begin(), gaps.end());
    auto pct = [&](double p) -> double { return gaps[min((int)gaps.size() - 1, (int)(gaps.size() * p))]; };
    double mean = accumulate(gaps.begin(), gaps.end(), 0.0) / gaps.size();
    rep << fixed << setprecision(2);
    rep << "帧间隔(ms): min=" << gaps.front() << "  p50=" << pct(0.50)
        << "  p90=" << pct(0.90) << "  p95=" << pct(0.95) << "  p99=" << pct(0.99)
        << "  max=" << gaps.back() << "  均值=" << mean << endl;
    rep << "等效更新帧率(按p50): " << 1000.0 / pct(0.50) << " fps"
        << "，(按均值): " << 1000.0 / mean << " fps" << endl;
    rep << "\n牌面就绪后最坏检测等待 ≈ p95帧间隔 " << pct(0.95) << " ms，平均 ≈ "
        << pct(0.50) / 2 << " ms；识别另需约0.2ms（常驻4线程）" << endl;
    rep << "\n帧间隔直方图(ms):" << endl;
    int bins[26] = {0};
    for (double g : gaps) { int b = (int)g; if (b > 25) b = 25; bins[b]++; }
    int maxBin = *max_element(bins, bins + 26);
    for (int b = 0; b <= 25; b++) {
        if (bins[b] == 0) continue;
        rep << "  " << setw(2) << b << "-" << setw(2) << (b + 1) << "ms |";
        int barLen = maxBin > 0 ? bins[b] * 40 / maxBin : 0;
        for (int k = 0; k < barLen; k++) rep << "#";
        rep << " " << bins[b] << endl;
    }
    string report = rep.str();
    cout << report;
    // 同时落盘（追加，带时间戳）到 exe 同目录
    time_t nowT = time(nullptr);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&nowT));
    ofstream rf(outPath, ios::app);
    rf << "\n---------- " << tbuf << " ----------" << endl;
    rf << report;
    rf.close();
    cout << "\n结果已追加保存到 " << outPath << endl;
    return 0;
}
