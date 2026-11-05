// 识别并行度基准（真实工作负载：extract二值化/归一化 + NCC匹配，4张合成牌）：
//  A. 单线程串行（当前主程序方案）
//  B. 常驻4线程 + condition_variable屏障（线程只创建一次）
//  C. 常驻4线程 + 自旋epoch屏障（最低延迟，监控期间占满4核）
//  D. 每轮新建4线程（对照，旧方案）
#define main disabled_24point_main
#include "../auto_24point.cpp"
#undef main

#include <thread>
#include <mutex>
#include <condition_variable>
#include <iomanip>

// ---- B: condition_variable 常驻池 ----
struct CvPool {
    Recognizer* r; vector<Mat>* cards = nullptr;
    vector<thread> w; vector<pair<string,float>> res{4};
    mutex m; condition_variable startCv, doneCv;
    int epoch = 0, done = 0; bool quit = false;
    CvPool(Recognizer* rr) : r(rr) {
        for (int i = 0; i < 4; i++) w.emplace_back([this, i] {
            int seen = 0;
            for (;;) {
                unique_lock<mutex> lk(m);
                startCv.wait(lk, [&] { return quit || epoch > seen; });
                if (quit) return;
                int e = epoch;
                lk.unlock();
                Mat num = r->extract((*cards)[i]);
                res[i] = r->match(num);
                lk.lock();
                done++;
                seen = e;
                if (done == 4) doneCv.notify_one();
            }
        });
    }
    void run(vector<Mat>& cs) {
        unique_lock<mutex> lk(m);
        cards = &cs; done = 0; epoch++;
        startCv.notify_all();
        doneCv.wait(lk, [&] { return done == 4; });
    }
    ~CvPool() {
        { lock_guard<mutex> lk(m); quit = true; epoch++; startCv.notify_all(); }
        for (auto& t : w) t.join();
    }
};

// ---- C: 自旋 epoch 常驻池（延迟下限，占满核）----
struct SpinPool {
    Recognizer* r; vector<Mat>* cards = nullptr;
    vector<thread> w; vector<pair<string,float>> res{4};
    atomic<int> epoch{0}, done{0}; atomic<bool> quit{false};
    SpinPool(Recognizer* rr) : r(rr) {
        for (int i = 0; i < 4; i++) w.emplace_back([this, i] {
            int seen = 0;
            while (!quit.load(std::memory_order_relaxed)) {
                int e = epoch.load(std::memory_order_acquire);
                if (e > seen) {
                    seen = e;
                    Mat num = r->extract((*cards)[i]);
                    res[i] = r->match(num);
                    done.fetch_add(1, std::memory_order_release);
                } else {
                    YieldProcessor();
                }
            }
        });
    }
    void run(vector<Mat>& cs) {
        cards = &cs;
        done.store(0, std::memory_order_relaxed);
        epoch.fetch_add(1, std::memory_order_release);
        while (done.load(std::memory_order_acquire) < 4) YieldProcessor();
    }
    ~SpinPool() { quit.store(true); for (auto& t : w) t.join(); }
};

int main(int argc, char** argv) {
    try {
        timeBeginPeriod(1);
        string dir = (argc > 1) ? argv[1] : "templates";
        Recognizer r; r.load(dir);

        // 4张白底黑字合成牌（同bench_ncc夹具）
        vector<Mat> cards(4);
        int gi[4] = {3, 17, 31, 44};
        for (int c = 0; c < 4; c++) {
            Mat card(208, 168, CV_8UC4, Scalar(255, 255, 255, 255));
            Mat dark, big, bg;
            bitwise_not(r.templates[gi[c]].image, dark);
            resize(dark, big, Size(120, 156), 0, 0, INTER_NEAREST);
            cvtColor(big, bg, COLOR_GRAY2BGRA);
            bg.copyTo(card(Rect(24, 26, 120, 156)));
            cards[c] = card;
        }
        LARGE_INTEGER freq, t0, t1; QueryPerformanceFrequency(&freq);
        const int R = 5000;
        auto msPerRound = [&](LARGE_INTEGER a, LARGE_INTEGER b) {
            return (double)(b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart / R;
        };

        // 正确性：常驻池结果必须与单线程串行完全一致
        {
            vector<pair<string, float>> ref(4);
            for (int c = 0; c < 4; c++) { Mat n = r.extract(cards[c]); ref[c] = r.match(n); }
            CvPool chk(&r);
            chk.run(cards);
            bool same = true;
            for (int c = 0; c < 4; c++) if (chk.res[c].first != ref[c].first) same = false;
            cout << "常驻池结果与单线程一致: " << (same ? "YES" : "NO") << " （识别为: ";
            for (int c = 0; c < 4; c++) cout << chk.res[c].first << " ";
            cout << "）" << endl;
        }

        // A 单线程串行
        QueryPerformanceCounter(&t0);
        for (int k = 0; k < R; k++) for (int c = 0; c < 4; c++) {
            Mat num = r.extract(cards[c]); r.match(num);
        }
        QueryPerformanceCounter(&t1);
        double msA = msPerRound(t0, t1);

        // B cv常驻池
        {
            CvPool pool(&r);
            pool.run(cards); // 预热（含线程首次唤醒）
            QueryPerformanceCounter(&t0);
            for (int k = 0; k < R; k++) pool.run(cards);
            QueryPerformanceCounter(&t1);
        }
        double msB = msPerRound(t0, t1);

        // C 自旋常驻池
        {
            SpinPool pool(&r);
            pool.run(cards);
            QueryPerformanceCounter(&t0);
            for (int k = 0; k < R; k++) pool.run(cards);
            QueryPerformanceCounter(&t1);
        }
        double msC = msPerRound(t0, t1);

        // D 每轮新建线程
        Mat resMat;
        QueryPerformanceCounter(&t0);
        for (int k = 0; k < R; k++) {
            vector<thread> th;
            for (int c = 0; c < 4; c++) th.emplace_back([&, c] {
                Mat num = r.extract(cards[c]);
                for (auto& t : r.templates) matchTemplate(num, t.image, resMat, TM_CCOEFF_NORMED);
            });
            for (auto& t : th) t.join();
        }
        QueryPerformanceCounter(&t1);
        double msD = msPerRound(t0, t1);

        cout << fixed << setprecision(4);
        cout << "=== 每轮(4牌 extract+识别)墙钟，" << R << "轮均值 ===" << endl;
        cout << "A 单线程串行(现状)        : " << msA << " ms" << endl;
        cout << "B 常驻4线程 cv屏障        : " << msB << " ms" << endl;
        cout << "C 常驻4线程 自旋屏障      : " << msC << " ms" << endl;
        cout << "D 每轮新建4线程(旧match)  : " << msD << " ms" << endl;
        cout << "C相对A: " << msA / msC << "x，净省 " << msA - msC << " ms/轮" << endl;
        timeEndPeriod(1);
        return 0;
    } catch (const exception& e) { cerr << "异常: " << e.what() << endl; return 1; }
}
