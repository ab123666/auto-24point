// 离线验证工具（不截屏、不点击）：
//  1) 模板留一法(Leave-One-Out)：每张模板图作为查询，从模板库临时移除自身后识别，
//     检验新归一化管线（固定40x56画布、零运行时resize）的标签可分性与Q/10消歧。
//  2) preciseSleepMs 精度：实测各档时长的均值/min/max，验证点击等待与参数严格一致。
// 编译: 同主程序链接 OpenCV 与系统库；本文件 #include 整个 auto_24point.cpp。
#define main disabled_24point_main
#include "../auto_24point.cpp"
#undef main

#include <iomanip>

int main(int argc, char** argv) {
    timeBeginPeriod(1); // 与主程序一致的1ms定时器精度
    try {
        string dir = (argc > 1) ? argv[1] : "templates";
        Recognizer r;
        r.load(dir);
        int n = (int)r.templates.size();
        if (n == 0) { cerr << "模板为空: " << dir << endl; return 1; }

        // 诊断：每个模板画布的尺寸/均值/标准差（std=0即零方差，会使NCC未定义）
        cout << "=== 模板画布诊断 ===" << endl;
        for (auto& t : r.templates) {
            Scalar m, sd;
            meanStdDev(t.image, m, sd);
            cout << "  " << t.label << "\tsize=" << t.image.cols << "x" << t.image.rows
                 << " nonzero=" << countNonZero(t.image)
                 << " mean=" << fixed << setprecision(1) << m[0]
                 << " std=" << setprecision(2) << sd[0] << endl;
        }

        // ---------- 1. 留一法 ----------
        map<string, int> total, correct;
        vector<tuple<string, string, float>> fails;
        for (int k = 0; k < n; k++) {
            Template keep = r.templates[k];
            r.templates.erase(r.templates.begin() + k);
            auto [lab, score] = r.match(keep.image);
            r.templates.insert(r.templates.begin() + k, keep);
            total[keep.label]++;
            if (lab == keep.label) {
                correct[keep.label]++;
            } else {
                fails.emplace_back(keep.label, lab, score);
            }
        }
        int sumOk = 0, sumAll = 0;
        cout << "=== 留一法识别验证（固定画布/零resize 管线）===" << endl;
        for (auto& [label, cnt] : total) {
            int ok = correct[label];
            sumOk += ok; sumAll += cnt;
            cout << "  " << label << ": " << ok << "/" << cnt;
            if (ok != cnt) cout << "  <<< 有误识别";
            cout << endl;
        }
        cout << "  合计: " << sumOk << "/" << sumAll;
        if (sumOk == sumAll) cout << "  全部通过" << endl; else cout << "  存在失败" << endl;
        if (!fails.empty()) {
            cout << "  失败明细（真实 -> 预测, 分数）:" << endl;
            for (auto& [gt, pred, score] : fails)
                cout << "    " << gt << " -> " << pred << "  (" << fixed << setprecision(3) << score << ")" << endl;
        }

        // ---------- 2. preciseSleepMs 精度 ----------
        cout << "=== preciseSleepMs 精度（每档50次，单位ms）===" << endl;
        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        for (int ms : {1, 16, 32, 60}) {
            const int N = 50;
            double mn = 1e9, mx = 0, sum = 0;
            for (int i = 0; i < N; i++) {
                QueryPerformanceCounter(&t0);
                preciseSleepMs(ms);
                QueryPerformanceCounter(&t1);
                double got = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
                mn = min(mn, got); mx = max(mx, got); sum += got;
            }
            cout << "  目标 " << setw(2) << ms << "ms : 均值 " << fixed << setprecision(3)
                 << setw(7) << sum / N << "   min " << setw(7) << mn
                 << "   max " << setw(7) << mx
                 << "   最大正误差 " << setw(6) << mx - ms << "ms" << endl;
        }

        // ---------- 3. 手写NCC：性能 + 与matchTemplate等价性 ----------
        cout << "=== 手写NCC（每轮=4牌×50模板，取3000轮均值）===" << endl;
        const int R = 3000;
        volatile float sink = 0; // 防止优化掉
        LARGE_INTEGER t0n, t1n;
        QueryPerformanceCounter(&t0n);
        for (int rr = 0; rr < R; ++rr) {
            for (int c = 0; c < 4; c++) {
                auto [lab, sc] = r.match(r.templates[(rr * 7 + c * 13) % n].image);
                sink = sc; (void)lab;
            }
        }
        QueryPerformanceCounter(&t1n);
        double nccMs = (double)(t1n.QuadPart - t0n.QuadPart) * 1000.0 / freq.QuadPart / R;
        cout << "  r.match 手写NCC: " << fixed << setprecision(4) << nccMs
             << " ms/轮, " << nccMs / 200 * 1000 << " μs/次" << endl;

        // 等价性：对每个模板查询，比较r.match分数与matchTemplate逐模板最高分
        Mat eqRes;
        double maxDiff = 0;
        for (int k = 0; k < n; k++) {
            const Mat& q = r.templates[k].image;
            auto [labNcc, scoreNcc] = r.match(q);
            float refBest = -2;
            for (auto& t : r.templates) {
                matchTemplate(q, t.image, eqRes, TM_CCOEFF_NORMED);
                refBest = max(refBest, eqRes.at<float>(0, 0));
            }
            maxDiff = max(maxDiff, (double)fabs(scoreNcc - refBest));
        }
        cout << "  与matchTemplate分数最大偏差: " << scientific << setprecision(2) << maxDiff;
        if (maxDiff < 1e-4) cout << "  等价性通过" << endl; else cout << "  <<< 偏差超阈值" << endl;

        cout << "=== 验证结束 ===" << endl;
        timeEndPeriod(1);
        return (sumOk == sumAll) ? 0 : 2;
    } catch (const exception& e) {
        timeEndPeriod(1);
        cerr << "异常: " << e.what() << endl;
        return 1;
    }
}
