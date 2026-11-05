// 生产类 RecognizePool 集成测试：
//  - 4张合成牌识别结果与单线程串行一致
//  - 连续1000轮派发/屏障无死锁
//  - 含空Mat（模拟越界牌位）不崩溃、该位返回空
//  - 析构join正常（进程能走到结束即通过）
#define main disabled_24point_main
#include "../auto_24point.cpp"
#undef main

#include <thread>

int main(int argc, char** argv) {
    try {
        timeBeginPeriod(1);
        string dir = (argc > 1) ? argv[1] : "templates";
        Recognizer r; r.load(dir);
        if (r.templates.empty()) { cerr << "无模板" << endl; return 1; }

        vector<Mat> cards(4);
        int gi[4] = {3, 17, 31, 44};
        for (int c = 0; c < 4; c++) {
            Mat card(220, 180, CV_8UC4, Scalar(255, 255, 255, 255));
            Mat dark, big, bg;
            bitwise_not(r.templates[gi[c]].image, dark);
            resize(dark, big, Size(128, 168), 0, 0, INTER_NEAREST);
            cvtColor(big, bg, COLOR_GRAY2BGRA);
            bg.copyTo(card(Rect(26, 26, 128, 168)));
            cards[c] = card;
        }

        // 单线程参考结果
        vector<pair<string, float>> ref(4);
        for (int c = 0; c < 4; c++) { Mat n = r.extract(cards[c]); ref[c] = r.match(n); }

        int failures = 0;
        {
            RecognizePool pool(&r); // 生产类：构造=4线程常驻
            for (int round = 0; round < 1000; round++) {
                vector<pair<string, float>> out(4);
                pool.recognize(cards, out);
                if (round == 0) {
                    cout << "池识别结果: ";
                    for (int c = 0; c < 4; c++) cout << out[c].first << "(" << fixed << setprecision(2) << out[c].second << ") ";
                    cout << endl;
                }
                for (int c = 0; c < 4; c++)
                    if (out[c].first != ref[c].first) failures++;
            }
            // 空Mat测试：第2位越界留空
            vector<Mat> withEmpty = cards; withEmpty[1] = Mat();
            vector<pair<string, float>> outE(4);
            pool.recognize(withEmpty, outE);
            if (!outE[1].first.empty()) { cerr << "空Mat应返回空标签" << endl; failures++; }
            if (outE[0].first != ref[0].first || outE[2].first != ref[2].first || outE[3].first != ref[3].first) {
                cerr << "含空位时其余牌识别错误" << endl; failures++;
            }
            cout << "空Mat（越界牌位）处理正常" << endl;
        } // 析构join，能走到这里说明无死锁

        cout << (failures == 0 ? "[PASS] RecognizePool 1000轮结果一致、空位安全、析构正常"
                               : "[FAIL] 不一致次数=" + to_string(failures)) << endl;
        timeEndPeriod(1);
        return failures == 0 ? 0 : 1;
    } catch (const exception& e) {
        cerr << "异常: " << e.what() << endl;
        return 1;
    }
}
