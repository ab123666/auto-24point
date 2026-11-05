// 识别路径微基准（不改主程序）：对比三种实现每轮(4牌×50模板=200次匹配)墙钟耗时
//  A. 现状：4线程 matchTemplate（含每轮创建/销毁4线程的开销）
//  B. 单线程 matchTemplate（固定40x56画布，等大1x1输出）
//  C. 单线程手写NCC：模板预计算零均值单位方差，查询归一化后Mat::dot点积
// 并校验 C 与 B 的分数/argmax标签一致性（数学上应完全等价）。
#define main disabled_24point_main
#include "../auto_24point.cpp"
#undef main

#include <iomanip>
#include <thread>

struct NormTpl { string label; Mat n; }; // CV_32F，零均值、L2范数=1

int main(int argc, char** argv) {
    try {
        timeBeginPeriod(1);
        string dir = (argc > 1) ? argv[1] : "templates";
        Recognizer r;
        r.load(dir);
        int n = (int)r.templates.size();

        // 预计算归一化模板（C方案，启动时一次性成本）
        vector<NormTpl> nt;
        for (auto& t : r.templates) {
            Mat f;
            t.image.convertTo(f, CV_32F);
            Scalar m; meanStdDev(f, m, noArray());
            f -= m[0];
            double ln = norm(f, NORM_L2);
            if (ln > 1e-9) f /= ln;
            nt.push_back({t.label, f});
        }

        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b, int rounds) {
            return (double)(b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart / rounds;
        };
        const int R = 3000;
        Mat res;

        // A. 现状：4线程 matchTemplate + 每轮创建线程
        vector<float> bestS(4); vector<string> bestL(4);
        QueryPerformanceCounter(&t0);
        for (int rr = 0; rr < R; ++rr) {
            vector<thread> th;
            for (int c = 0; c < 4; c++) {
                th.emplace_back([&, c]() {
                    const Mat& q = r.templates[(rr * 7 + c * 13) % n].image;
                    float bs = -1; string bl;
                    for (auto& t : r.templates) {
                        matchTemplate(q, t.image, res, TM_CCOEFF_NORMED);
                        float s = res.at<float>(0, 0);
                        if (s > bs) { bs = s; bl = t.label; }
                    }
                    bestS[c] = bs; bestL[c] = bl;
                });
            }
            for (auto& x : th) x.join();
        }
        QueryPerformanceCounter(&t1);
        double msA = ms(t0, t1, R);

        // B. 单线程 matchTemplate
        QueryPerformanceCounter(&t0);
        for (int rr = 0; rr < R; ++rr) {
            for (int c = 0; c < 4; c++) {
                const Mat& q = r.templates[(rr * 7 + c * 13) % n].image;
                for (auto& t : r.templates) matchTemplate(q, t.image, res, TM_CCOEFF_NORMED);
            }
        }
        QueryPerformanceCounter(&t1);
        double msB = ms(t0, t1, R);

        // C. 单线程手写NCC点积（含每牌一次查询归一化预处理）
        thread_local Mat fq, centered;
        QueryPerformanceCounter(&t0);
        for (int rr = 0; rr < R; ++rr) {
            for (int c = 0; c < 4; c++) {
                const Mat& q = r.templates[(rr * 7 + c * 13) % n].image;
                q.convertTo(fq, CV_32F);
                Scalar mq; meanStdDev(fq, mq, noArray());
                fq -= mq[0];
                double lq = norm(fq, NORM_L2);
                if (lq > 1e-9) fq /= lq;
                for (auto& t : nt) {
                    float s = (float)fq.dot(t.n);
                    (void)s;
                }
            }
        }
        QueryPerformanceCounter(&t1);
        double msC = ms(t0, t1, R);

        cout << "=== 每轮(4牌x" << n << "模板)墙钟耗时，" << R << "轮均值 ===" << endl;
        cout << fixed << setprecision(4);
        cout << "A 现状 4线程matchTemplate(含建线程): " << msA << " ms" << endl;
        cout << "B 单线程 matchTemplate            : " << msB << " ms" << endl;
        cout << "C 单线程 手写NCC点积(含预处理)     : " << msC << " ms" << endl;
        cout << "C相对A加速: " << msA / msC << "x" << endl;

        // 一致性校验：50个查询，C与B逐模板分数最大偏差、argmax标签是否一致
        double maxDiff = 0; int argMismatch = 0;
        for (int k = 0; k < n; k++) {
            const Mat& q = r.templates[k].image;
            Mat f; q.convertTo(f, CV_32F);
            Scalar mq; meanStdDev(f, mq, noArray());
            f -= mq[0];
            double lq = norm(f, NORM_L2);
            f /= lq;
            float bBest = -2, cBest = -2; string bLab, cLab;
            for (int j = 0; j < n; j++) {
                matchTemplate(q, r.templates[j].image, res, TM_CCOEFF_NORMED);
                float sb = res.at<float>(0, 0);
                float sc = (float)f.dot(nt[j].n);
                maxDiff = max(maxDiff, (double)fabs(sb - sc));
                if (sb > bBest) { bBest = sb; bLab = r.templates[j].label; }
                if (sc > cBest) { cBest = sc; cLab = nt[j].label; }
            }
            if (bLab != cLab) { argMismatch++; cout << "  argmax不一致: q=" << r.templates[k].label
                                                   << " B=" << bLab << " C=" << cLab << endl; }
        }
        cout << "=== 一致性：分数最大偏差 " << setprecision(6) << maxDiff
             << "，argmax标签不一致数 " << argMismatch << "/" << n << " ===" << endl;

        // ===== 端到端：含extract(二值化+中值滤波+归一化)，4张合成牌 =====
        // 用模板字形合成168x208 BGRA牌面（黑底白字放大居中），模拟真实截图
        vector<Mat> cards(4);
        int glyphIdx[4] = {3, 17, 31, 44};
        for (int c = 0; c < 4; c++) {
            // 真实牌面为白底黑字：模板(黑底白字)反相后放大居中，贴到白色168x208牌面
            Mat card(208, 168, CV_8UC4, Scalar(255, 255, 255, 255));
            Mat dark, big, bigBgra;
            bitwise_not(r.templates[glyphIdx[c]].image, dark);
            resize(dark, big, Size(120, 156), 0, 0, INTER_NEAREST);
            cvtColor(big, bigBgra, COLOR_GRAY2BGRA);
            bigBgra.copyTo(card(Rect(24, 26, 120, 156)));
            cards[c] = card;
        }

        // D. 旧端到端：4线程 extract + matchTemplate
        QueryPerformanceCounter(&t0);
        for (int rr = 0; rr < R; ++rr) {
            vector<thread> th;
            for (int c = 0; c < 4; c++) {
                th.emplace_back([&, c]() {
                    Mat num = r.extract(cards[c]);
                    for (auto& t : r.templates)
                        matchTemplate(num, t.image, res, TM_CCOEFF_NORMED);
                });
            }
            for (auto& x : th) x.join();
        }
        QueryPerformanceCounter(&t1);
        double msD = ms(t0, t1, R);

        // E. 新端到端：单线程 extract + 手写NCC match
        QueryPerformanceCounter(&t0);
        for (int rr = 0; rr < R; ++rr) {
            for (int c = 0; c < 4; c++) {
                Mat num = r.extract(cards[c]);
                r.match(num);
            }
        }
        QueryPerformanceCounter(&t1);
        double msE = ms(t0, t1, R);

        // 正确性：新端到端识别4张合成牌的标签
        cout << "=== 端到端(含extract) " << R << "轮均值 ===" << endl;
        cout << "D 旧 4线程 extract+matchTemplate: " << fixed << setprecision(4) << msD << " ms" << endl;
        cout << "E 新 单线程 extract+手写NCC     : " << fixed << setprecision(4) << msE << " ms" << endl;
        cout << "加速: " << fixed << setprecision(2) << msD / msE << "x | 合成牌识别结果: ";
        for (int c = 0; c < 4; c++) {
            Mat num = r.extract(cards[c]);
            auto [lab, sc] = r.match(num);
            cout << r.templates[glyphIdx[c]].label << "->" << lab << "(" << fixed << setprecision(2) << sc << ") ";
        }
        cout << endl;

        timeEndPeriod(1);
        return 0;
    } catch (const exception& e) {
        cerr << "异常: " << e.what() << endl;
        return 1;
    }
}
