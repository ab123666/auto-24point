// 火拼24点自动答题 - C++极速版
// 编译: g++ -O2 -std=c++17 auto_24point.cpp -o auto_24point.exe -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lgdi32 -luser32
// 或用 MSVC: cl /O2 /std:c++17 auto_24point.cpp /link opencv_world.lib gdi32.lib user32.lib

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <opencv2/opencv.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cmath>
#include <filesystem>
#include <climits>

namespace fs = std::filesystem;
using namespace cv;
using namespace std;

// ============ 配置 ============
struct CalibProfile {
    vector<Point> cardCenters;
    map<char, Point> opButtons;
    Size cardCrop = Size(160, 200);
    bool initialized = false; // 用户是否设置过
};

struct Config {
    // 当前激活的校准数据（代码其他地方直接用这些字段）
    vector<Point> cardCenters;
    map<char, Point> opButtons;
    Size cardCrop = Size(160, 200);

    // 4套校准方案
    CalibProfile profiles[4];
    int currentProfile = 0;

    // 全局设置（所有方案共享）
    int recognizeDelayMs = 50;  // 识别→开始执行鼠标序列的延迟
    int moveDelayMs = 0;         // 每次移动后等待时间（移动→按下）
    int mouseDownMs = 1;         // 鼠标按下保持时间（按下→弹起）

    // 自动切题（全局，所有方案共享）：每次答题序列执行完后，
    // 再点击 advancePoints 中的一个点，按触发轮次 1..5 循环
    bool autoAdvance = false;        // 开关
    vector<Point> advancePoints;     // 固定5个切题点
};

// 自动切题5个点的默认位置（屏幕下1/5处水平均布），仅在用户未设置时兜底
vector<Point> defaultAdvancePoints() {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    vector<Point> pts;
    for (int i = 0; i < 5; i++) pts.push_back(Point(sw * (i + 1) / 6, sh * 4 / 5));
    return pts;
}

// 把当前激活字段同步到当前方案
void syncActiveToProfile(Config& cfg) {
    cfg.profiles[cfg.currentProfile].cardCenters = cfg.cardCenters;
    cfg.profiles[cfg.currentProfile].opButtons = cfg.opButtons;
    cfg.profiles[cfg.currentProfile].cardCrop = cfg.cardCrop;
    cfg.profiles[cfg.currentProfile].initialized = true;
}

// 切换方案
void switchProfile(Config& cfg, int idx) {
    if (idx < 0 || idx >= 4) return;
    syncActiveToProfile(cfg); // 先保存当前
    cfg.currentProfile = idx;
    CalibProfile& p = cfg.profiles[idx];
    if (p.initialized) {
        cfg.cardCenters = p.cardCenters;
        cfg.opButtons = p.opButtons;
        cfg.cardCrop = p.cardCrop;
    } else {
        // 未设置过的方案用默认排布
        cfg.cardCenters = {{400, 300}, {600, 300}, {400, 500}, {600, 500}};
        cfg.opButtons = {{'+', {800, 250}}, {'-', {900, 350}}, {'*', {800, 450}}, {'/', {900, 550}}};
        cfg.cardCrop = Size(160, 200);
    }
}

// 全局鼠标延迟（fastClick用）
int g_moveDelayMs = 0;
int g_mouseDownMs = 1;

// 屏幕尺寸缓存（避免每次点击都调用GetSystemMetrics）
int g_screenW = 0;
int g_screenH = 0;

// 高精度等待：实际时长严格等于ms（主程序已timeBeginPeriod(1)，实测误差<0.01ms）。
// Sleep粗等（受调度影响可能偏长），最后10ms用QueryPerformanceCounter自旋补齐，
// 保证不会睡过目标时刻。
// ms<=0时立即返回——绝不调用Sleep(0)（Sleep(0)会主动让出时间片，引入不可控延迟）。
void preciseSleepMs(int ms) {
    if (ms <= 0) return;
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    LONGLONG targetTicks = freq.QuadPart * ms / 1000;
    LONGLONG spinTicks = freq.QuadPart * 10 / 1000; // 最后10ms自旋，覆盖常见调度抖动，避免睡过目标
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG elapsed = now.QuadPart - t0.QuadPart;
        LONGLONG remain = targetTicks - elapsed;
        if (remain <= 0) break;
        if (remain > spinTicks) Sleep(1); // 1ms定时器精度下Sleep(1)最坏约2ms，不会睡过
    }
}

// ============ 极简JSON解析（只解析需要的格式） ============
string readFile(const string& path) {
    ifstream f(path);
    if (!f) return "";
    stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 解析 "key": [x, y] 格式的坐标
bool parsePointPair(const string& json, const string& key, int& x, int& y) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == string::npos) return false;
    pos = json.find('[', pos);
    if (pos == string::npos) return false;
    size_t end = json.find(']', pos);
    string arr = json.substr(pos + 1, end - pos - 1);
    sscanf_s(arr.c_str(), "%d , %d", &x, &y);
    return true;
}

// 从JSON子串解析一个方案
void parseProfile(const string& json, CalibProfile& prof) {
    // card_centers
    size_t pos = json.find("\"card_centers\"");
    if (pos != string::npos) {
        pos = json.find('[', pos);
        int depth = 0;
        size_t start = pos;
        for (size_t i = pos; i < json.size(); i++) {
            if (json[i] == '[') depth++;
            if (json[i] == ']') { depth--; if (depth == 0) { pos = i; break; } }
        }
        string arr = json.substr(start, pos - start + 1);
        size_t p = 1;
        while (p < arr.size()) {
            size_t lb = arr.find('[', p);
            if (lb == string::npos) break;
            size_t rb = arr.find(']', lb);
            string pair = arr.substr(lb + 1, rb - lb - 1);
            int x, y;
            sscanf_s(pair.c_str(), "%d , %d", &x, &y);
            prof.cardCenters.push_back(Point(x, y));
            p = rb + 1;
        }
    }
    // op_buttons
    pos = json.find("\"op_buttons\"");
    if (pos != string::npos) {
        for (char op : {'+', '-', '*', '/'}) {
            string key = string("\"") + op + "\"";
            size_t opPos = json.find(key, pos);
            if (opPos != string::npos) {
                size_t lb = json.find('[', opPos);
                size_t rb = json.find(']', lb);
                string pair = json.substr(lb + 1, rb - lb - 1);
                int x, y;
                sscanf_s(pair.c_str(), "%d , %d", &x, &y);
                prof.opButtons[op] = Point(x, y);
            }
        }
    }
    // card_crop
    int cw, ch;
    if (parsePointPair(json, "card_crop", cw, ch)) {
        prof.cardCrop = Size(cw, ch);
    }
    // initialized
    size_t ipos = json.find("\"initialized\"");
    if (ipos != string::npos) {
        size_t colon = json.find(':', ipos);
        if (colon != string::npos) {
            prof.initialized = (json.substr(colon + 1, 4) == "true");
        }
    }
    if (!prof.cardCenters.empty()) prof.initialized = true;
}

Config loadConfig(const string& path) {
    Config cfg;
    string json = readFile(path);
    if (json.empty()) return cfg;

    // 检测新格式（有profiles字段）
    bool newFormat = json.find("\"profiles\"") != string::npos;

    if (newFormat) {
        // 解析 current_profile
        size_t cpos = json.find("\"current_profile\"");
        if (cpos != string::npos) {
            size_t colon = json.find(':', cpos);
            if (colon != string::npos) {
                cfg.currentProfile = atoi(json.c_str() + colon + 1);
                if (cfg.currentProfile < 0 || cfg.currentProfile > 3) cfg.currentProfile = 0;
            }
        }
        // 解析每个方案
        size_t profPos = json.find("\"profiles\"");
        size_t arrStart = json.find('[', profPos);
        int depth = 0;
        size_t arrEnd = arrStart;
        for (size_t i = arrStart; i < json.size(); i++) {
            if (json[i] == '[') depth++;
            if (json[i] == ']') { depth--; if (depth == 0) { arrEnd = i; break; } }
        }
        // 逐个解析 { ... }
        size_t p = arrStart + 1;
        int idx = 0;
        while (p < arrEnd && idx < 4) {
            size_t lb = json.find('{', p);
            if (lb == string::npos || lb > arrEnd) break;
            depth = 0;
            size_t rb = lb;
            for (size_t i = lb; i < arrEnd; i++) {
                if (json[i] == '{') depth++;
                if (json[i] == '}') { depth--; if (depth == 0) { rb = i; break; } }
            }
            string profJson = json.substr(lb, rb - lb + 1);
            parseProfile(profJson, cfg.profiles[idx]);
            idx++;
            p = rb + 1;
        }
    } else {
        // 旧格式：加载到方案0
        parseProfile(json, cfg.profiles[0]);
        cfg.profiles[0].initialized = true;
        cfg.currentProfile = 0;
    }

    // 激活当前方案
    CalibProfile& p = cfg.profiles[cfg.currentProfile];
    if (p.initialized) {
        cfg.cardCenters = p.cardCenters;
        cfg.opButtons = p.opButtons;
        cfg.cardCrop = p.cardCrop;
    } else {
        cfg.cardCenters = {{400, 300}, {600, 300}, {400, 500}, {600, 500}};
        cfg.opButtons = {{'+', {800, 250}}, {'-', {900, 350}}, {'*', {800, 450}}, {'/', {900, 550}}};
        cfg.cardCrop = Size(160, 200);
    }

    // 解析全局延迟设置
    size_t dpos = json.find("\"recognize_delay_ms\"");
    if (dpos != string::npos) {
        size_t colon = json.find(':', dpos);
        if (colon != string::npos) cfg.recognizeDelayMs = atoi(json.c_str() + colon + 1);
    }
    size_t cpos = json.find("\"move_delay_ms\"");
    if (cpos != string::npos) {
        size_t colon = json.find(':', cpos);
        if (colon != string::npos) cfg.moveDelayMs = atoi(json.c_str() + colon + 1);
    } else {
        size_t oldpos = json.find("\"click_interval_ms\"");
        if (oldpos != string::npos) {
            size_t colon = json.find(':', oldpos);
            if (colon != string::npos) cfg.moveDelayMs = atoi(json.c_str() + colon + 1);
        }
    }
    size_t mpos = json.find("\"mouse_down_ms\"");
    if (mpos != string::npos) {
        size_t colon = json.find(':', mpos);
        if (colon != string::npos) cfg.mouseDownMs = atoi(json.c_str() + colon + 1);
    }

    // 解析自动切题设置（旧配置文件无此字段时保持默认：关闭+默认5点）
    size_t apos = json.find("\"auto_advance\"");
    if (apos != string::npos) {
        size_t segEnd = json.find('}', apos); // 对象内只有数组[]，第一个}即对象闭合
        string seg = (segEnd != string::npos) ? json.substr(apos, segEnd - apos) : json.substr(apos);
        size_t ep = seg.find("\"enabled\"");
        if (ep != string::npos) {
            size_t colon = seg.find(':', ep);
            if (colon != string::npos) {
                size_t v = colon + 1;
                while (v < seg.size() && (seg[v] == ' ' || seg[v] == '\t' || seg[v] == '\n' || seg[v] == '\r')) v++;
                cfg.autoAdvance = (seg.substr(v, 4) == "true");
            }
        }
        size_t pp = seg.find("\"points\"");
        if (pp != string::npos) {
            size_t lb = seg.find('[', pp);
            if (lb != string::npos) {
                int depth = 0;
                size_t rb = lb;
                for (size_t i = lb; i < seg.size(); i++) {
                    if (seg[i] == '[') depth++;
                    if (seg[i] == ']') { depth--; if (depth == 0) { rb = i; break; } }
                }
                string arr = seg.substr(lb, rb - lb + 1);
                size_t q = 1;
                while (q < arr.size() && (int)cfg.advancePoints.size() < 5) {
                    size_t xb = arr.find('[', q);
                    if (xb == string::npos) break;
                    size_t xr = arr.find(']', xb);
                    int x, y;
                    if (sscanf_s(arr.substr(xb + 1, xr - xb - 1).c_str(), "%d , %d", &x, &y) == 2)
                        cfg.advancePoints.push_back(Point(x, y));
                    q = xr + 1;
                }
            }
        }
    }
    if ((int)cfg.advancePoints.size() < 5) {
        vector<Point> def = defaultAdvancePoints();
        while ((int)cfg.advancePoints.size() < 5)
            cfg.advancePoints.push_back(def[cfg.advancePoints.size()]);
    }

    return cfg;
}

void saveConfig(const string& path, const Config& cfg) {
    ofstream f(path);
    if (!f) return;
    f << "{" << endl;
    f << "  \"current_profile\": " << cfg.currentProfile << "," << endl;
    f << "  \"profiles\": [" << endl;
    for (int i = 0; i < 4; i++) {
        const CalibProfile& p = cfg.profiles[i];
        f << "    {" << endl;
        f << "      \"initialized\": " << (p.initialized ? "true" : "false") << "," << endl;
        f << "      \"card_centers\": [";
        for (size_t j = 0; j < p.cardCenters.size(); j++) {
            f << "[" << p.cardCenters[j].x << "," << p.cardCenters[j].y << "]";
            if (j < p.cardCenters.size() - 1) f << ",";
        }
        f << "]," << endl;
        f << "      \"op_buttons\": {";
        char ops[] = {'+', '-', '*', '/'};
        bool first = true;
        for (int j = 0; j < 4; j++) {
            auto it = p.opButtons.find(ops[j]);
            if (it != p.opButtons.end()) {
                if (!first) f << ",";
                f << "\"" << ops[j] << "\": [" << it->second.x << "," << it->second.y << "]";
                first = false;
            }
        }
        f << "}," << endl;
        f << "      \"card_crop\": [" << p.cardCrop.width << "," << p.cardCrop.height << "]" << endl;
        f << "    }";
        if (i < 3) f << ",";
        f << endl;
    }
    f << "  ]," << endl;
    f << "  \"recognize_delay_ms\": " << cfg.recognizeDelayMs << "," << endl;
    f << "  \"move_delay_ms\": " << cfg.moveDelayMs << "," << endl;
    f << "  \"mouse_down_ms\": " << cfg.mouseDownMs << "," << endl;
    // 自动切题（始终写满5个点，不足部分用默认位置补齐）
    vector<Point> adv = cfg.advancePoints;
    vector<Point> defAdv = defaultAdvancePoints();
    while ((int)adv.size() < 5) adv.push_back(defAdv[adv.size()]);
    f << "  \"auto_advance\": {" << endl;
    f << "    \"enabled\": " << (cfg.autoAdvance ? "true" : "false") << "," << endl;
    f << "    \"points\": [";
    for (int i = 0; i < 5; i++) {
        f << "[" << adv[i].x << "," << adv[i].y << "]";
        if (i < 4) f << ",";
    }
    f << "]" << endl;
    f << "  }" << endl;
    f << "}" << endl;
    f.close();
}

// ============ 模板识别 ============
struct Template {
    string label;
    Mat image; // 固定40x56归一化画布（二值字形，诊断/消歧用）
    Mat norm;  // CV_32F零均值、L2范数=1的模板，match时与查询做点积（启动时预计算一次）
};

class Recognizer {
public:
    vector<Template> templates;
    Size templateSize = Size(40, 56);
    float matchThreshold = 0.60f;
    static const int MAX_TEMPLATES_PER_LABEL = 999;

    void load(const string& dir) {
        templates.clear();
        map<string, vector<Mat>> byLabel;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() != ".png") continue;
            string stem = entry.path().stem().string();
            string label = stem.substr(0, stem.find('_'));
            Mat img = imread(entry.path().string(), IMREAD_GRAYSCALE);
            if (img.empty()) continue;
            // 与截图识别完全相同的归一化口径：二值化->紧框(含pad)->等比居中到固定画布
            Mat bin;
            threshold(img, bin, 100, 255, THRESH_BINARY);
            // 模板源图可能是白底黑字（如早期截图），统一反相为黑底白字，
            // 否则全白模板与任何查询的NCC都未定义（OpenCV返回1.0），会碾压所有标签
            double whiteRatio = (double)countNonZero(bin) / (bin.rows * bin.cols);
            if (whiteRatio > 0.5) bitwise_not(bin, bin);
            Mat glyph = normalizeGlyph(bin);
            if (!glyph.empty()) byLabel[label].push_back(glyph);
        }
        for (auto& [label, imgs] : byLabel) {
            int cnt = min((int)imgs.size(), MAX_TEMPLATES_PER_LABEL);
            for (int i = 0; i < cnt; i++) {
                Template t;
                t.label = label;
                t.image = imgs[i];
                t.norm = makeUnitTemplate(imgs[i]);
                templates.push_back(t);
            }
        }
        cout << "模板库: " << templates.size() << " 个 (全模板匹配)" << endl;
    }

    Mat resizeKeepRatio(const Mat& img, int tw, int th) {
        thread_local Mat resized, canvas;
        double scale = min((double)tw / img.cols, (double)th / img.rows);
        int nw = max(1, (int)(img.cols * scale));
        int nh = max(1, (int)(img.rows * scale));
        resize(img, resized, Size(nw, nh), 0, 0, INTER_AREA);
        canvas = Mat::zeros(th, tw, CV_8U);
        int xoff = (tw - nw) / 2, yoff = (th - nh) / 2;
        resized.copyTo(canvas(Rect(xoff, yoff, nw, nh)));
        return canvas.clone(); // clone：thread_local缓冲会被下次调用覆写，必须返回独立副本
    }

    // 二值字形 -> 紧框(四周pad像素) -> 等比居中到固定40x56画布。
    // 模板加载与截图识别共用这一条归一化口径，保证两边字形大小/位置一致。
    Mat normalizeGlyph(const Mat& binary) {
        Mat coords;
        findNonZero(binary, coords);
        if (coords.empty()) return Mat();
        Rect r = boundingRect(coords);
        const int pad = 4;
        int x = max(0, r.x - pad), y = max(0, r.y - pad);
        int rw = min(binary.cols - x, r.width + pad * 2);
        int rh = min(binary.rows - y, r.height + pad * 2);
        Mat glyph = binary(Rect(x, y, rw, rh)).clone();
        return resizeKeepRatio(glyph, templateSize.width, templateSize.height);
    }

    // 画布 -> CV_32F、减均值、L2归一化为单位向量。
    // 两个零均值单位向量的点积就是 TM_CCOEFF_NORMED（等大1x1时数学完全等价），
    // 模板侧在加载时预计算一次，运行时每牌只需做一次查询归一化+若干次点积。
    Mat makeUnitTemplate(const Mat& canvas) {
        Mat f;
        canvas.convertTo(f, CV_32F);
        Scalar m;
        meanStdDev(f, m, noArray());
        f -= m[0];
        double ln = norm(f, NORM_L2);
        if (ln > 1e-9) f /= ln;
        return f;
    }

    Mat extract(const Mat& card) {
        thread_local Mat gray, binary;
        int h = card.rows, w = card.cols;
        int cx1 = (int)(w * 0.10), cx2 = (int)(w * 0.90);
        int cy1 = (int)(h * 0.10), cy2 = (int)(h * 0.90);
        Mat center = card(Rect(cx1, cy1, cx2 - cx1, cy2 - cy1)); // 引用，不分配
        cvtColor(center, gray, COLOR_BGRA2GRAY);
        threshold(gray, binary, 185, 255, THRESH_BINARY_INV);
        medianBlur(binary, binary, 3);
        // 与模板加载同一归一化口径，输出固定40x56画布
        return normalizeGlyph(binary);
    }

    // numberImg为extract输出的固定40x56归一化画布，模板同为40x56。
    // 手写NCC点积：查询零均值/L2归一化后与预计算模板点积，
    // 与等大matchTemplate(TM_CCOEFF_NORMED)数学等价（实测分数偏差<1e-6），快约50倍。
    pair<string, float> match(const Mat& numberImg) {
        if (numberImg.empty() || numberImg.size() != templateSize) return {"", 0.0f};

        Mat f;
        numberImg.convertTo(f, CV_32F);
        Scalar mq;
        meanStdDev(f, mq, noArray());
        f -= mq[0];
        double lq = norm(f, NORM_L2);
        if (lq <= 1e-9) return {"", 0.0f}; // 零方差（全黑/全白），NCC未定义，按不匹配处理
        f /= lq;

        string bestLabel, secondLabel;
        float bestScore = -1, secondScore = -1;

        for (auto& t : templates) {
            float score = (float)f.dot(t.norm);
            if (score > bestScore) {
                secondScore = bestScore;
                secondLabel = bestLabel;
                bestScore = score;
                bestLabel = t.label;
            } else if (score > secondScore) {
                secondScore = score;
                secondLabel = t.label;
            }
        }

        // Q/10消歧：在固定画布上求字形外接框的宽高比
        float aspect = 1.0f;
        Mat coords;
        findNonZero(numberImg, coords);
        if (!coords.empty()) {
            Rect r = boundingRect(coords);
            if (r.height > 0) aspect = (float)r.width / r.height;
        }
        if ((bestLabel == "Q" || bestLabel == "10") &&
            (secondLabel == "Q" || secondLabel == "10")) {
            if (aspect > 1.25f && bestLabel == "Q") bestLabel = "10";
            else if (aspect < 1.15f && bestLabel == "10") bestLabel = "Q";
        }

        return {bestLabel, bestScore};
    }

    int labelToValue(const string& label) {
        if (label == "A") return 1;
        if (label == "J") return 11;
        if (label == "Q") return 12;
        if (label == "K") return 13;
        try { return stoi(label); } catch (...) { return -1; }
    }
};

// ============ 常驻识别线程池 ============
// 4个worker各守一个牌位，进入监控会话时只创建一次、之后一直常驻等待；
// 每帧由主线程（DXGI截图有公寓线程要求，只能在主线程做）截好4张牌后派发，
// 4线程并行做 extract+match，全部完成后主线程继续。实测比单线程串行快约2.5倍
// （0.44ms→0.17ms/轮）。worker用条件变量阻塞等待，不截图时空闲、不占CPU。
class RecognizePool {
public:
    RecognizePool(Recognizer* rec) : r(rec) {
        for (int i = 0; i < 4; i++)
            workers.emplace_back(&RecognizePool::workerLoop, this, i);
    }
    ~RecognizePool() {
        { lock_guard<mutex> lk(m); quit = true; epoch++; startCv.notify_all(); }
        for (auto& t : workers) if (t.joinable()) t.join();
    }
    // 并行识别4张牌，结果写入out[0..3]（标签,置信度）
    void recognize(const vector<Mat>& cards, vector<pair<string, float>>& out) {
        unique_lock<mutex> lk(m);
        in = &cards; res = &out;
        done = 0;
        epoch++;
        startCv.notify_all();
        doneCv.wait(lk, [&] { return done == 4; });
    }
private:
    void workerLoop(int id) {
        int seen = 0;
        for (;;) {
            unique_lock<mutex> lk(m);
            startCv.wait(lk, [&] { return quit || epoch > seen; });
            if (quit) return;
            int myEpoch = epoch;
            const Mat& card = (*in)[id];
            lk.unlock();
            // extract 的工作缓冲是 thread_local、match 的归一化图为栈局部、
            // 模板norm只读——4线程互不共享可写状态
            pair<string, float> ans;
            if (!card.empty()) {
                Mat num = r->extract(card);
                if (!num.empty()) ans = r->match(num);
            }
            lk.lock();
            (*res)[id] = ans;
            done++;
            seen = myEpoch;
            if (done == 4) doneCv.notify_one();
        }
    }
    Recognizer* r;
    vector<thread> workers;
    mutex m;
    condition_variable startCv, doneCv;
    const vector<Mat>* in = nullptr;
    vector<pair<string, float>>* res = nullptr;
    int epoch = 0, done = 0;
    bool quit = false;
};

// ============ DXGI桌面复制截图（比GDI快2-5倍） ============
class DXGICapture {
public:
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* regionTex = nullptr; // 按截图区域复用的Staging纹理（CPU可读）
    int regionW = 0, regionH = 0;         // regionTex当前尺寸
    int screenW = 0, screenH = 0;
    bool initialized = false;

    bool init() {
        if (initialized) return true;
        // 创建D3D11设备
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                0, nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
            if (FAILED(hr)) return false;
        }
        // 获取DXGI设备和适配器
        IDXGIDevice* dxgiDevice = nullptr;
        device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        IDXGIAdapter* adapter = nullptr;
        dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&adapter);
        IDXGIOutput* output = nullptr;
        adapter->EnumOutputs(0, &output);
        // 初始化输出复制
        IDXGIOutput1* output1 = nullptr;
        output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        hr = output1->DuplicateOutput(device, &duplication);
        output1->Release();
        output->Release();
        adapter->Release();
        dxgiDevice->Release();
        if (FAILED(hr)) return false;
        // 获取屏幕尺寸
        screenW = GetSystemMetrics(SM_CXSCREEN);
        screenH = GetSystemMetrics(SM_CYSCREEN);
        // Staging纹理改为首次截图时按实际区域大小创建（见ensureRegionTexture），
        // 不再创建全屏纹理，避免每帧在GPU/CPU间搬运整块桌面。
        initialized = true;
        return true;
    }

    // 按截图区域创建（或尺寸变化时重建）Staging纹理，区域固定时只创建一次
    bool ensureRegionTexture(int w, int h) {
        if (regionTex && regionW == w && regionH == h) return true;
        if (regionTex) { regionTex->Release(); regionTex = nullptr; }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &regionTex);
        if (FAILED(hr)) { regionTex = nullptr; return false; }
        regionW = w;
        regionH = h;
        return true;
    }

    // 截取指定区域，返回BGRA格式Mat
    // 只把该区域从桌面纹理拷到小尺寸Staging纹理（CopySubresourceRegion），
    // GPU拷贝和Map回读的数据量都从"整块屏幕"降到"包围盒大小"。
    Mat captureRegion(int x, int y, int w, int h) {
        if (!initialized) return Mat();
        // 防御性裁剪到屏幕范围，保证srcBox合法
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        w = min(w, screenW - x);
        h = min(h, screenH - y);
        if (w <= 0 || h <= 0) return Mat();

        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        HRESULT hr = duplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
        if (FAILED(hr)) return Mat();
        ID3D11Texture2D* desktopTex = nullptr;
        desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTex);

        if (!ensureRegionTexture(w, h)) {
            desktopTex->Release();
            desktopResource->Release();
            duplication->ReleaseFrame();
            return Mat();
        }
        D3D11_BOX srcBox;
        srcBox.left = x;
        srcBox.top = y;
        srcBox.front = 0;
        srcBox.right = x + w;
        srcBox.bottom = y + h;
        srcBox.back = 1;
        context->CopySubresourceRegion(regionTex, 0, 0, 0, 0, desktopTex, 0, &srcBox);
        desktopTex->Release();
        desktopResource->Release();

        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context->Map(regionTex, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) { duplication->ReleaseFrame(); return Mat(); }
        // 从Staging纹理回读到Mat（BGRA 32位）；RowPitch通常对齐大于w*4，按行拷贝
        Mat img(h, w, CV_8UC4);
        uint8_t* src = (uint8_t*)mapped.pData;
        if ((SIZE_T)mapped.RowPitch == (SIZE_T)w * 4) {
            memcpy(img.data, src, (size_t)w * 4 * h);
        } else {
            for (int row = 0; row < h; row++) {
                memcpy(img.data + (size_t)row * w * 4,
                       src + (size_t)row * mapped.RowPitch, (size_t)w * 4);
            }
        }
        context->Unmap(regionTex, 0);
        duplication->ReleaseFrame();
        return img;
    }

    void release() {
        if (regionTex) regionTex->Release();
        if (duplication) duplication->Release();
        if (context) context->Release();
        if (device) device->Release();
        regionTex = nullptr;
        regionW = regionH = 0;
        initialized = false;
    }
};

DXGICapture g_dxgi;

// ============ 屏幕截图（BitBlt极速版） ============
Mat captureScreen() {
    HDC hScreen = GetDC(nullptr);
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, w, h);
    SelectObject(hMem, hBmp);
    BitBlt(hMem, 0, 0, w, h, hScreen, 0, 0, SRCCOPY);

    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = -h; // top-down
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;

    Mat img(h, w, CV_8UC3);
    GetDIBits(hMem, hBmp, 0, h, img.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    // 保持BGR格式，和Python版一致（extract中用COLOR_BGR2GRAY）

    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
    return img;
}

// 只截指定区域（比全屏快很多）
Mat captureRegion(int x, int y, int w, int h) {
    // Windows BMP要求每行4字节对齐，宽度向上取整到4的倍数
    int wAligned = (w + 3) & ~3;
    HDC hScreen = GetDC(nullptr);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, wAligned, h);
    SelectObject(hMem, hBmp);
    BitBlt(hMem, 0, 0, w, h, hScreen, x, y, SRCCOPY);

    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = wAligned;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;

    Mat img(h, wAligned, CV_8UC3);
    GetDIBits(hMem, hBmp, 0, h, img.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    // 保持BGR格式，和Python版一致

    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);

    // 裁剪回实际宽度
    return img(Rect(0, 0, w, h)).clone();
}

// ============ 极速鼠标点击 ============
// 时序严格按用户参数，无任何额外延迟：
//   MOVE注入 -> 精确等待 moveDelayMs -> DOWN注入 -> 精确等待 mouseDownMs -> UP注入
// （UP到下一步MOVE之间参数定义为0ms，代码中也确实没有任何等待）
void fastClick(int x, int y) {
    int sw = g_screenW > 0 ? g_screenW : GetSystemMetrics(SM_CXSCREEN);
    int sh = g_screenH > 0 ? g_screenH : GetSystemMetrics(SM_CYSCREEN);
    int nx = (int)(x * 65535.0 / sw);
    int ny = (int)(y * 65535.0 / sh);
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, nx, ny, 0, 0);
    preciseSleepMs(g_moveDelayMs); // 移动->按下，严格等于moveDelayMs（0=立即）
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    preciseSleepMs(g_mouseDownMs); // 按下->弹起，严格等于mouseDownMs（0=立即）
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
}

void fastMove(int x, int y) {
    int sw = g_screenW > 0 ? g_screenW : GetSystemMetrics(SM_CXSCREEN);
    int sh = g_screenH > 0 ? g_screenH : GetSystemMetrics(SM_CYSCREEN);
    int nx = (int)(x * 65535.0 / sw);
    int ny = (int)(y * 65535.0 / sh);
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, nx, ny, 0, 0);
}

// ============ 24点求解（实时计算，不依赖JSON缓存） ============
struct Solution {
    string structure;
    vector<char> ops;
    vector<int> indices;
    string expr;
};

// 预计算缓存：整数key -> 最优解（vector直接寻址，比unordered_map快）
vector<Solution> g_solutionCache;
bool g_cacheLoaded = false;

// 排序后的4个数字(1-13)编码为0-28560的索引
inline int encodeKey(int a, int b, int c, int d) {
    return (a-1)*2197 + (b-1)*169 + (c-1)*13 + (d-1);
}

// 4元素排序网络（5次比较交换，比std::sort快）
inline void sort4(int& a, int& b, int& c, int& d) {
    #define SWAP(x,y) if(x>y){int t=x;x=y;y=t;}
    SWAP(a,b); SWAP(c,d); SWAP(a,c); SWAP(b,d); SWAP(b,c);
    #undef SWAP
}

// 解析JSON缓存文件
void loadSolutionCache(const string& path) {
    g_solutionCache.assign(28561, Solution()); // 28561 = 13^4
    ifstream f(path);
    if (!f) { cout << "  [缓存] 未找到预计算文件" << endl; return; }
    string json((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    f.close();

    int count = 0;
    size_t pos = 0;
    while ((pos = json.find("\"[", pos)) != string::npos) {
        size_t keyEnd = json.find("]", pos);
        if (keyEnd == string::npos) break;
        string keyStr = json.substr(pos + 2, keyEnd - pos - 2); // a, b, c, d（跳过"和[）
        // 解析4个数字
        int nums[4]; int ni = 0;
        stringstream kss(keyStr); char comma;
        while (kss >> nums[ni]) { kss >> comma; ni++; if(ni>=4) break; }
        if (ni < 4) { pos = keyEnd+1; continue; }
        int idx = encodeKey(nums[0], nums[1], nums[2], nums[3]);

        size_t valStart = json.find("{", keyEnd);
        if (valStart == string::npos) break;
        int depth = 0;
        size_t valEnd = valStart;
        for (; valEnd < json.size(); valEnd++) {
            if (json[valEnd] == '{') depth++;
            else if (json[valEnd] == '}') { depth--; if (depth == 0) break; }
        }
        string val = json.substr(valStart, valEnd - valStart + 1);

        Solution sol;
        size_t sp = val.find("\"structure\":");
        if (sp != string::npos) {
            size_t q1 = val.find('"', sp + 12);
            size_t q2 = val.find('"', q1 + 1);
            sol.structure = val.substr(q1 + 1, q2 - q1 - 1);
        }
        size_t op = val.find("\"ops\":");
        if (op != string::npos) {
            size_t b1 = val.find('[', op);
            size_t b2 = val.find(']', b1);
            string opsStr = val.substr(b1 + 1, b2 - b1 - 1);
            for (char c : opsStr) {
                if (c == '+' || c == '-' || c == '*' || c == '/') sol.ops.push_back(c);
            }
        }
        size_t ip = val.find("\"indices\":");
        if (ip != string::npos) {
            size_t b1 = val.find('[', ip);
            size_t b2 = val.find(']', b1);
            string idxStr = val.substr(b1 + 1, b2 - b1 - 1);
            stringstream ss(idxStr);
            int x; char comma2;
            while (ss >> x) { sol.indices.push_back(x); ss >> comma2; }
        }
        size_t ep = val.find("\"expr\":");
        if (ep != string::npos) {
            size_t q1 = val.find('"', ep + 7);
            size_t q2 = val.find('"', q1 + 1);
            sol.expr = val.substr(q1 + 1, q2 - q1 - 1);
        }

        if (sol.ops.size() == 3 && sol.indices.size() == 4) {
            // 枚举24种位置排列，全部预填入vector（查表时零排序）
            int perm[4] = {0,1,2,3};
            int s[4] = {nums[0], nums[1], nums[2], nums[3]};
            do {
                // perm[j] = 排序位j对应的原始位置
                // 逆排列：invPerm[原始位置] = 排序位
                int invPerm[4];
                for (int j = 0; j < 4; j++) invPerm[perm[j]] = j;
                // 原始位置j的数字 = s[invPerm[j]]
                int key = encodeKey(s[invPerm[0]], s[invPerm[1]], s[invPerm[2]], s[invPerm[3]]);
                Solution psol = sol;
                // 排序位 → 原始位置：perm[排序位]
                psol.indices[0] = perm[sol.indices[0]];
                psol.indices[1] = perm[sol.indices[1]];
                psol.indices[2] = perm[sol.indices[2]];
                psol.indices[3] = perm[sol.indices[3]];
                g_solutionCache[key] = psol;
            } while (next_permutation(perm, perm+4));
            count++;
        }
        pos = valEnd + 1;
    }
    g_cacheLoaded = true;
    cout << "  [缓存] 已加载 " << count << " 个预计算解 (vector直接寻址)" << endl;
}

// 查表获取最优解（零排序，直接用原始位置编码）
bool lookupSolution(const vector<int>& values, Solution& out) {
    if (!g_cacheLoaded) return false;
    int idx = encodeKey(values[0], values[1], values[2], values[3]);
    if (g_solutionCache[idx].structure.empty()) return false;
    out = g_solutionCache[idx]; // indices已经是原始位置，无需映射
    return true;
}

vector<Solution> solve24(const vector<int>& cards) {
    vector<Solution> solutions;
    set<string> seen;
    vector<char> OPS = {'+', '-', '*', '/'};

    auto calc = [](double a, double b, char op) -> double {
        switch (op) {
            case '+': return a + b;
            case '-': return a - b;
            case '*': return a * b;
            case '/': return b != 0 ? a / b : 1e9;
        }
        return 0;
    };
    auto is24 = [](double v) { return abs(v - 24) < 1e-6; };

    vector<int> idx = {0, 1, 2, 3};
    do {
        int i0 = idx[0], i1 = idx[1], i2 = idx[2], i3 = idx[3];
        double v0 = cards[i0], v1 = cards[i1], v2 = cards[i2], v3 = cards[i3];
        string L0 = to_string(cards[i0]), L1 = to_string(cards[i1]);
        string L2 = to_string(cards[i2]), L3 = to_string(cards[i3]);

        for (char op1 : OPS) for (char op2 : OPS) for (char op3 : OPS) {
            // 1. left_nested: ((a op1 b) op2 c) op3 d  (7步，最优)
            if (is24(calc(calc(calc(v0,v1,op1),v2,op2),v3,op3))) {
                string e = "((" + L0 + " " + op1 + " " + L1 + ") " + op2 + " " + L2 + ") " + op3 + " " + L3;
                if (seen.insert(e).second)
                    solutions.push_back({"left_nested", {op1,op2,op3}, {i0,i1,i2,i3}, e});
            }
            // 2. mixed1: (a op1 (b op2 c)) op3 d  (8步)
            if (is24(calc(calc(v0, calc(v1,v2,op2), op1), v3, op3))) {
                string e = "(" + L0 + " " + op1 + " (" + L1 + " " + op2 + " " + L2 + ")) " + op3 + " " + L3;
                if (seen.insert(e).second)
                    solutions.push_back({"mixed1", {op1,op2,op3}, {i0,i1,i2,i3}, e});
            }
            // 3. mixed2: a op1 ((b op2 c) op3 d)  (8步)
            if (is24(calc(v0, calc(calc(v1,v2,op2),v3,op3), op1))) {
                string e = L0 + " " + op1 + " ((" + L1 + " " + op2 + " " + L2 + ") " + op3 + " " + L3 + ")";
                if (seen.insert(e).second)
                    solutions.push_back({"mixed2", {op1,op2,op3}, {i0,i1,i2,i3}, e});
            }
            // 4. right_nested: a op1 (b op2 (c op3 d))  (9步)
            if (is24(calc(v0, calc(v1, calc(v2,v3,op3), op2), op1))) {
                string e = L0 + " " + op1 + " (" + L1 + " " + op2 + " (" + L2 + " " + op3 + " " + L3 + "))";
                if (seen.insert(e).second)
                    solutions.push_back({"right_nested", {op1,op2,op3}, {i0,i1,i2,i3}, e});
            }
            // 5. split: (a op1 b) op2 (c op3 d)  (9步)
            if (is24(calc(calc(v0,v1,op1), calc(v2,v3,op3), op2))) {
                string e = "(" + L0 + " " + op1 + " " + L1 + ") " + op2 + " (" + L2 + " " + op3 + " " + L3 + ")";
                if (seen.insert(e).second)
                    solutions.push_back({"split", {op1,op2,op3}, {i0,i1,i2,i3}, e});
            }
        }
    } while (next_permutation(idx.begin(), idx.end()));

    // 按步数排序：left_nested(7) < mixed1/mixed2(8) < right_nested/split(9)
    auto priority = [](const string& s) {
        if (s == "left_nested") return 0;
        if (s == "mixed1" || s == "mixed2") return 1;
        return 2;
    };
    sort(solutions.begin(), solutions.end(), [&](const Solution& a, const Solution& b) {
        return priority(a.structure) < priority(b.structure);
    });

    return solutions;
}

// 根据方案生成点击序列
vector<pair<char, Point>> buildSequence(const Solution& sol, const vector<Point>& cardCenters, const map<char, Point>& opButtons) {
    vector<pair<char, Point>> seq;
    int i0 = sol.indices[0], i1 = sol.indices[1];
    int i2 = sol.indices[2], i3 = sol.indices[3];
    char op1 = sol.ops[0], op2 = sol.ops[1], op3 = sol.ops[2];

    if (sol.structure == "left_nested") {
        // ((a op1 b) op2 c) op3 d
        seq.push_back({'card', cardCenters[i0]});
        seq.push_back({'op', opButtons.at(op1)});
        seq.push_back({'card', cardCenters[i1]});
        seq.push_back({'op', opButtons.at(op2)});
        seq.push_back({'card', cardCenters[i2]});
        seq.push_back({'op', opButtons.at(op3)});
        seq.push_back({'card', cardCenters[i3]});
    } else if (sol.structure == "mixed1") {
        // (a op1 (b op2 c)) op3 d
        seq.push_back({'card', cardCenters[i1]});
        seq.push_back({'op', opButtons.at(op2)});
        seq.push_back({'card', cardCenters[i2]});
        seq.push_back({'card', cardCenters[i0]});
        seq.push_back({'op', opButtons.at(op1)});
        seq.push_back({'card', cardCenters[i2]});
        seq.push_back({'op', opButtons.at(op3)});
        seq.push_back({'card', cardCenters[i3]});
    } else if (sol.structure == "mixed2") {
        // a op1 ((b op2 c) op3 d)
        seq.push_back({'card', cardCenters[i1]});
        seq.push_back({'op', opButtons.at(op2)});
        seq.push_back({'card', cardCenters[i2]});
        seq.push_back({'op', opButtons.at(op3)});
        seq.push_back({'card', cardCenters[i3]});
        seq.push_back({'card', cardCenters[i0]});
        seq.push_back({'op', opButtons.at(op1)});
        seq.push_back({'card', cardCenters[i3]});
    } else if (sol.structure == "right_nested") {
        // a op1 (b op2 (c op3 d))
        seq.push_back({'card', cardCenters[i2]});
        seq.push_back({'op', opButtons.at(op3)});
        seq.push_back({'card', cardCenters[i3]});
        seq.push_back({'card', cardCenters[i1]});
        seq.push_back({'op', opButtons.at(op2)});
        seq.push_back({'card', cardCenters[i3]});
        seq.push_back({'card', cardCenters[i0]});
        seq.push_back({'op', opButtons.at(op1)});
        seq.push_back({'card', cardCenters[i3]});
    } else { // split
        // (a op1 b) op2 (c op3 d)
        seq.push_back({'card', cardCenters[i0]});
        seq.push_back({'op', opButtons.at(op1)});
        seq.push_back({'card', cardCenters[i1]});
        seq.push_back({'card', cardCenters[i2]});
        seq.push_back({'op', opButtons.at(op3)});
        seq.push_back({'card', cardCenters[i3]});
        seq.push_back({'card', cardCenters[i1]});
        seq.push_back({'op', opButtons.at(op2)});
        seq.push_back({'card', cardCenters[i3]});
    }
    return seq;
}

// ============ 可视化校准（Win32分层窗口） ============
static HWND g_overlayHwnd = NULL;
static Config* g_calibCfg = NULL;
static int g_dragIndex = -1;
static POINT g_dragOffset = {0, 0};
static bool g_calibDone = false;
static const int CALIB_ALPHA = 180;

Point& getCalibPoint(int idx) {
    if (idx < 4) return g_calibCfg->cardCenters[idx];
    static char ops[] = {'+', '-', '*', '/'};
    return g_calibCfg->opButtons[ops[idx - 4]];
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH hBrush = CreateSolidBrush(RGB(20, 20, 30));
            FillRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);
            int cw = g_calibCfg->cardCrop.width;
            int ch = g_calibCfg->cardCrop.height;
            for (int i = 0; i < 4; i++) {
                Point& p = g_calibCfg->cardCenters[i];
                RECT r = {p.x - cw/2, p.y - ch/2, p.x + cw/2, p.y + ch/2};
                HPEN hPen = CreatePen(PS_SOLID, 3, RGB(255, 80, 80));
                HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
                HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, r.left, r.top, r.right, r.bottom);
                SelectObject(hdc, hOldPen);
                SelectObject(hdc, hOldBrush);
                DeleteObject(hPen);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 100));
                HFONT hFont = CreateFontA(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
                HFONT hOld = (HFONT)SelectObject(hdc, hFont);
                char buf[32];
                sprintf_s(buf, "C%d", i + 1);
                TextOutA(hdc, r.left + 4, r.top + 2, buf, (int)strlen(buf));
                SelectObject(hdc, hOld);
                DeleteObject(hFont);
            }
            char opLabels[] = {'+', '-', '*', '/'};
            for (int i = 0; i < 4; i++) {
                Point& pt = g_calibCfg->opButtons[opLabels[i]];
                RECT r = {pt.x - 30, pt.y - 30, pt.x + 30, pt.y + 30};
                HPEN hPen = CreatePen(PS_SOLID, 3, RGB(80, 255, 80));
                HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
                HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, r.left, r.top, r.right, r.bottom);
                SelectObject(hdc, hOldPen);
                SelectObject(hdc, hOldBrush);
                DeleteObject(hPen);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(100, 255, 100));
                HFONT hFont = CreateFontA(36, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
                HFONT hOld = (HFONT)SelectObject(hdc, hFont);
                char buf[8] = {opLabels[i], 0};
                TextOutA(hdc, pt.x - 10, pt.y - 18, buf, 1);
                SelectObject(hdc, hOld);
                DeleteObject(hFont);
            }
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            HFONT hTipFont = CreateFontA(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
            HFONT hOldTip = (HFONT)SelectObject(hdc, hTipFont);
            const char* tip = "Drag to move | Wheel to resize card | Enter=Save | Esc=Cancel";
            TextOutA(hdc, 15, 15, tip, (int)strlen(tip));
            SelectObject(hdc, hOldTip);
            DeleteObject(hTipFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = {LOWORD(lParam), HIWORD(lParam)};
            int cw = g_calibCfg->cardCrop.width;
            int ch = g_calibCfg->cardCrop.height;
            for (int i = 0; i < 4; i++) {
                Point& p = g_calibCfg->cardCenters[i];
                if (abs(pt.x - p.x) <= cw/2 + 15 && abs(pt.y - p.y) <= ch/2 + 15) {
                    g_dragIndex = i;
                    g_dragOffset.x = pt.x - p.x;
                    g_dragOffset.y = pt.y - p.y;
                    SetCapture(hwnd);
                    return 0;
                }
            }
            char ops[] = {'+', '-', '*', '/'};
            for (int i = 0; i < 4; i++) {
                Point& p = g_calibCfg->opButtons[ops[i]];
                if (abs(pt.x - p.x) <= 50 && abs(pt.y - p.y) <= 50) {
                    g_dragIndex = 4 + i;
                    g_dragOffset.x = pt.x - p.x;
                    g_dragOffset.y = pt.y - p.y;
                    SetCapture(hwnd);
                    return 0;
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_dragIndex >= 0) {
                POINT pt = {LOWORD(lParam), HIWORD(lParam)};
                Point& p = getCalibPoint(g_dragIndex);
                p.x = pt.x - g_dragOffset.x;
                p.y = pt.y - g_dragOffset.y;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_dragIndex >= 0) {
                g_dragIndex = -1;
                ReleaseCapture();
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int step = delta > 0 ? 4 : -4;
            g_calibCfg->cardCrop.width = max(60, g_calibCfg->cardCrop.width + step);
            g_calibCfg->cardCrop.height = max(80, g_calibCfg->cardCrop.height + step);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_RETURN) { g_calibDone = true; DestroyWindow(hwnd); return 0; }
            if (wParam == VK_ESCAPE) { g_calibDone = false; DestroyWindow(hwnd); return 0; }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool runCalibration(Config& cfg) {
    g_calibCfg = &cfg;
    g_dragIndex = -1;
    g_calibDone = false;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "24PointCalibOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_SIZEALL);
    RegisterClassA(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    g_overlayHwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOPMOST, "24PointCalibOverlay", "24点校准", WS_POPUP, 0, 0, sw, sh, NULL, NULL, GetModuleHandle(NULL), NULL);
    SetLayeredWindowAttributes(g_overlayHwnd, 0, CALIB_ALPHA, LWA_ALPHA);
    ShowWindow(g_overlayHwnd, SW_SHOW);
    SetForegroundWindow(g_overlayHwnd);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    UnregisterClassA("24PointCalibOverlay", GetModuleHandle(NULL));
    return g_calibDone;
}

// ============ 自动切题点可视化设置（Win32分层窗口，交互同校准） ============
static vector<Point>* g_advPoints = nullptr;
static int g_advDragIndex = -1;
static POINT g_advDragOffset = {0, 0};
static bool g_advDone = false;
static HWND g_advHwnd = NULL;
static const int ADV_ALPHA = 180;

LRESULT CALLBACK AdvanceOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH hBrush = CreateSolidBrush(RGB(20, 20, 30));
            FillRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);
            for (int i = 0; i < 5; i++) {
                Point& pt = (*g_advPoints)[i];
                RECT r = {pt.x - 30, pt.y - 30, pt.x + 30, pt.y + 30};
                HPEN hPen = CreatePen(PS_SOLID, 3, RGB(80, 255, 80));
                HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
                HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, r.left, r.top, r.right, r.bottom);
                SelectObject(hdc, hOldPen);
                SelectObject(hdc, hOldBrush);
                DeleteObject(hPen);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(100, 255, 100));
                HFONT hFont = CreateFontA(36, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
                HFONT hOld = (HFONT)SelectObject(hdc, hFont);
                char buf[8];
                sprintf_s(buf, "%d", i + 1);
                TextOutA(hdc, pt.x - 10, pt.y - 18, buf, (int)strlen(buf));
                SelectObject(hdc, hOld);
                DeleteObject(hFont);
            }
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            HFONT hTipFont = CreateFontA(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
            HFONT hOldTip = (HFONT)SelectObject(hdc, hTipFont);
            const char* tip = "Drag points 1-5 to target positions | Enter=Save | Esc=Cancel";
            TextOutA(hdc, 15, 15, tip, (int)strlen(tip));
            SelectObject(hdc, hOldTip);
            DeleteObject(hTipFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = {LOWORD(lParam), HIWORD(lParam)};
            for (int i = 0; i < 5; i++) {
                Point& p = (*g_advPoints)[i];
                if (abs(pt.x - p.x) <= 50 && abs(pt.y - p.y) <= 50) {
                    g_advDragIndex = i;
                    g_advDragOffset.x = pt.x - p.x;
                    g_advDragOffset.y = pt.y - p.y;
                    SetCapture(hwnd);
                    return 0;
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_advDragIndex >= 0) {
                POINT pt = {LOWORD(lParam), HIWORD(lParam)};
                Point& p = (*g_advPoints)[g_advDragIndex];
                p.x = pt.x - g_advDragOffset.x;
                p.y = pt.y - g_advDragOffset.y;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_advDragIndex >= 0) {
                g_advDragIndex = -1;
                ReleaseCapture();
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_RETURN) { g_advDone = true; DestroyWindow(hwnd); return 0; }
            if (wParam == VK_ESCAPE) { g_advDone = false; DestroyWindow(hwnd); return 0; }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 可视化设置5个切题点。Enter保存返回true；Esc取消返回false并还原进入时的坐标
bool runAdvanceCalibration(vector<Point>& pts) {
    vector<Point> def = defaultAdvancePoints();
    while ((int)pts.size() < 5) pts.push_back(def[pts.size()]);
    vector<Point> backup = pts;
    g_advPoints = &pts;
    g_advDragIndex = -1;
    g_advDone = false;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = AdvanceOverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "24PointAdvanceOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_SIZEALL);
    RegisterClassA(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    g_advHwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOPMOST, "24PointAdvanceOverlay", "Advance Points", WS_POPUP, 0, 0, sw, sh, NULL, NULL, GetModuleHandle(NULL), NULL);
    SetLayeredWindowAttributes(g_advHwnd, 0, ADV_ALPHA, LWA_ALPHA);
    ShowWindow(g_advHwnd, SW_SHOW);
    SetForegroundWindow(g_advHwnd);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    UnregisterClassA("24PointAdvanceOverlay", GetModuleHandle(NULL));
    if (!g_advDone) pts = backup; // 取消则还原
    return g_advDone;
}

// ============ 全局状态 ============
atomic<bool> stopRequested(false);

// 键盘钩子触发标志（独立线程设置，主线程消费）
atomic<bool> g_cTriggered(false);
atomic<bool> g_vTriggered(false);
atomic<bool> g_escTriggered(false);
HHOOK g_keyboardHook = NULL;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        if (p->vkCode == 'C') g_cTriggered = true;
        if (p->vkCode == 'V') g_vTriggered = true;
        if (p->vkCode == VK_ESCAPE) g_escTriggered = true;
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

DWORD WINAPI KeyboardHookThread(LPVOID) {
    g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

void startKeyboardHook() {
    CreateThread(NULL, 0, KeyboardHookThread, NULL, 0, NULL);
}

// ============ 自动监控 ============
void runMonitor(Config& cfg, Recognizer& recognizer) {
    int roundCount = 0;
    bool monitoring = false;
    bool firstRound = true;
    // 自动切题：下一次答题后要点的切题点下标（0..4循环）。
    // 进入监控/按V/按Esc后都从第1个点重新开始。
    int advanceIdx = 0;

    // 常驻识别线程池：整个监控会话只创建一次，4个worker各守一个牌位并行 extract+match；
    // 条件变量阻塞等待，不识别时空闲不占CPU。实测每帧识别 0.44ms→0.17ms（约2.5倍）。
    RecognizePool recognizePool(&recognizer);

    // 【关键】进入监控前清空所有键盘标志，避免之前操作残留的C/V/Esc误触发
    g_cTriggered = false;
    g_vTriggered = false;
    g_escTriggered = false;

    while (true) {
        // 检查键盘钩子触发的标志（独立线程设置，不受Sleep影响）
        if (g_cTriggered.exchange(false)) {
            monitoring = true;
            cout << "\n[C键] 开始自动监控" << endl;
        }
        if (g_vTriggered.exchange(false)) {
            if (monitoring) {
                monitoring = false;
                advanceIdx = 0; // V停止后，下次答题从第1个切题点重新开始
                cout << "\n[V键] 停止监控，按C继续" << endl;
            }
        }
        if (g_escTriggered.exchange(false)) {
            cout << "\n[Esc] 返回菜单" << endl;
            break;
        }

        if (!monitoring) {
            Sleep(20);
            continue;
        }

        // 计算4张牌包围盒（加padding），只截这个区域
        const int PAD = 20;
        int minX = INT_MAX, minY = INT_MAX, maxX = 0, maxY = 0;
        for (int i = 0; i < 4; i++) {
            Point c = cfg.cardCenters[i];
            int x1 = c.x - cfg.cardCrop.width / 2;
            int y1 = c.y - cfg.cardCrop.height / 2;
            int x2 = x1 + cfg.cardCrop.width;
            int y2 = y1 + cfg.cardCrop.height;
            minX = min(minX, x1); minY = min(minY, y1);
            maxX = max(maxX, x2); maxY = max(maxY, y2);
        }
        minX = max(0, minX - PAD);
        minY = max(0, minY - PAD);
        maxX = min(GetSystemMetrics(SM_CXSCREEN), maxX + PAD);
        maxY = min(GetSystemMetrics(SM_CYSCREEN), maxY + PAD);
        int rw = maxX - minX, rh = maxY - minY;
        if (firstRound) {
            cout << "  截图区域: (" << minX << "," << minY << ") " << rw << "x" << rh << endl;
            firstRound = false;
        }
        Mat full = g_dxgi.captureRegion(minX, minY, rw, rh);
        if (full.empty()) continue; // DXGI截图失败，跳过这一帧

        // 准备4张牌的ROI（越界的留空Mat，worker会跳过）
        vector<Mat> cards(4);
        for (int i = 0; i < 4; i++) {
            Point c = cfg.cardCenters[i];
            int x = c.x - cfg.cardCrop.width / 2 - minX;
            int y = c.y - cfg.cardCrop.height / 2 - minY;
            if (x < 0 || y < 0 || x + cfg.cardCrop.width > rw || y + cfg.cardCrop.height > rh) {
                continue;
            }
            cards[i] = full(Rect(x, y, cfg.cardCrop.width, cfg.cardCrop.height));
        }

        // 常驻4线程并行识别（线程在进入监控时已创建，这里只做屏障派发，无建线程开销）
        vector<pair<string, float>> recs(4);
        recognizePool.recognize(cards, recs);

        vector<int> values(4);
        vector<float> confs(4);
        vector<bool> oks(4, false);
        for (int i = 0; i < 4; i++) {
            const string& label = recs[i].first;
            float conf = recs[i].second;
            if (label.empty() || conf < recognizer.matchThreshold) continue;
            int val = recognizer.labelToValue(label);
            if (val < 0) continue;
            values[i] = val;
            confs[i] = conf;
            oks[i] = true;
        }

        bool allOk = oks[0] && oks[1] && oks[2] && oks[3];
        if (!allOk) continue;
        float minConf = min({confs[0], confs[1], confs[2], confs[3]});

        Solution sol;
        if (!lookupSolution(values, sol)) continue; // 查不到=无解，直接跳过
        auto seq = buildSequence(sol, cfg.cardCenters, cfg.opButtons);
        roundCount++;
        cout << "\n[自动触发 #" << roundCount << "] " << values[0] << " " << values[1] << " "
             << values[2] << " " << values[3] << " (置信度=" << fixed << setprecision(3) << minConf << ")" << endl;
        cout << "  方案: " << sol.expr << endl;

        preciseSleepMs(cfg.recognizeDelayMs); // 严格等于识别延迟参数（0=立即，不会Sleep(0)让出时间片）
        if (g_vTriggered.load() || g_escTriggered.load()) continue; // 延迟期间被中断，跳过本轮点击

        POINT startPt;
        GetCursorPos(&startPt);

        // 点击序列期间临时提高线程优先级，减少调度抢占导致的按压时长正误差；
        // 序列结束或中断后立即恢复（不改变任何时序参数，只保证参数被严格执行）
        HANDLE hClickThread = GetCurrentThread();
        int savedPriority = GetThreadPriority(hClickThread);
        SetThreadPriority(hClickThread, THREAD_PRIORITY_HIGHEST);

        bool seqCompleted = true; // 原序列是否完整跑完（被V中断则为false，不执行切题点击）
        for (size_t step = 0; step < seq.size(); step++) {
            if (g_vTriggered.exchange(false)) {
                monitoring = false;
                seqCompleted = false;
                advanceIdx = 0; // V中断后，下次答题从第1个切题点重新开始
                SetThreadPriority(hClickThread, savedPriority);
                cout << "\n[V键] 停止监控" << endl;
                break;
            }
            if (g_escTriggered.exchange(false)) {
                SetThreadPriority(hClickThread, savedPriority);
                cout << "\n[Esc] 中断序列，返回菜单" << endl;
                goto monitorEnd;
            }
            auto [kind, pt] = seq[step];
            fastClick(pt.x, pt.y);
            // 弹起后0ms，直接下一次移动（移动延迟和按下延迟在fastClick内控制）
        }

        // 自动切题：原序列完整跑完后，按轮次循环追加一次切题点点击。
        // 这一步就是原序列的"第X+1步"：同样走fastClick，移动等待/按下保持完全沿用延迟设置。
        if (seqCompleted && cfg.autoAdvance && (int)cfg.advancePoints.size() >= 5) {
            if (g_vTriggered.exchange(false)) {
                monitoring = false;
                advanceIdx = 0;
                SetThreadPriority(hClickThread, savedPriority);
                cout << "\n[V键] 停止监控" << endl;
            } else if (g_escTriggered.exchange(false)) {
                SetThreadPriority(hClickThread, savedPriority);
                cout << "\n[Esc] 中断序列，返回菜单" << endl;
                goto monitorEnd;
            } else {
                int useIdx = advanceIdx;
                Point ap = cfg.advancePoints[useIdx];
                fastClick(ap.x, ap.y);
                advanceIdx = (advanceIdx + 1) % 5;
                cout << "  [切题] 追加点击第 " << (useIdx + 1) << "/5 点 (" << ap.x << "," << ap.y << ")" << endl;
            }
        }

        fastMove(startPt.x, startPt.y);
        SetThreadPriority(hClickThread, savedPriority);
        cout << "  执行完毕！" << endl;
    }

monitorEnd:
    cout << "\n共完成 " << roundCount << " 轮" << endl;
}

// ============ 主流程 ============
int main() {
    try {
    SetProcessDPIAware();
    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1); // 提高计时器精度，让Sleep更精准
    setNumThreads(4); // OpenCV多线程

    string baseDir = fs::current_path().string();
    string configPath = baseDir + "\\24point_config.json";
    string templateDir = baseDir + "\\templates";

    cout << "火拼24 自动答题 C++极速版" << endl;
    cout << string(50, '=') << endl;

    Config cfg = loadConfig(configPath);

    if (cfg.cardCenters.size() < 4) {
        cout << "未找到配置，进入可视化校准..." << endl;
        cfg.cardCenters = {{400, 300}, {600, 300}, {400, 500}, {600, 500}};
        cfg.opButtons = {{'+', {800, 250}}, {'-', {900, 350}}, {'*', {800, 450}}, {'/', {900, 550}}};
        cfg.cardCrop = Size(160, 200);
        bool ok = runCalibration(cfg);
        if (!ok) {
            cout << "校准取消，退出" << endl;
            system("pause");
            return 1;
        }
        syncActiveToProfile(cfg);
        saveConfig(configPath, cfg);
        cout << "配置已保存！" << endl;
    }

    cout << "牌位置: ";
    for (auto& p : cfg.cardCenters) cout << "(" << p.x << "," << p.y << ") ";
    cout << endl;
    cout << "裁剪尺寸: " << cfg.cardCrop.width << "x" << cfg.cardCrop.height << endl;
    cout << "识别延迟: " << cfg.recognizeDelayMs << "ms | 移动等待: " << cfg.moveDelayMs << "ms | 按下保持: " << cfg.mouseDownMs << "ms (弹起=0ms)" << endl;
    cout << "自动切题: " << (cfg.autoAdvance ? "开启（每次答题后循环点击切题点 1→5）" : "关闭") << endl;
    g_moveDelayMs = cfg.moveDelayMs;
    g_mouseDownMs = cfg.mouseDownMs;

    Recognizer recognizer;
    recognizer.load(templateDir);
    if (recognizer.templates.empty()) {
        cout << "错误: 没有模板" << endl;
        system("pause");
        return 1;
    }

    // 加载预计算解缓存
    string cachePath = baseDir + "\\24point_permutations.json";
    loadSolutionCache(cachePath);

    // 初始化DXGI截图
    if (g_dxgi.init()) {
        cout << "  [截图] DXGI桌面复制已初始化 (" << g_dxgi.screenW << "x" << g_dxgi.screenH << ")" << endl;
    } else {
        cout << "  [截图] DXGI初始化失败，回退GDI" << endl;
    }

    // 启动键盘钩子（独立线程，实时捕获C/V/Esc）
    startKeyboardHook();
    cout << "  [键盘] 全局钩子已启动 (C=开始, V=停止, Esc=返回)" << endl;

    while (true) {
        cout << "\n当前校准方案: " << (cfg.currentProfile + 1) << " / 4" << endl;
        cout << "模式:" << endl;
        cout << "  1. 自动监控" << endl;
        cout << "  2. 切换校准方案" << endl;
        cout << "  3. 修改当前校准方案（可视化校准）" << endl;
        cout << "  4. 延迟设置" << endl;
        cout << "  5. 自动切题（" << (cfg.autoAdvance ? "已开启" : "已关闭") << "）" << endl;
        cout << "  0. 退出" << endl;
        cout << "选择 (0-5): ";
        string choice;
        getline(cin, choice);
        if (choice.empty()) continue;
        char c = choice[0];

        if (c == '1') {
            cout << "\n就绪！" << endl;
            cout << "  按 C 键 -> 开始自动监控" << endl;
            cout << "  按 V 键 -> 停止监控" << endl;
            cout << "  按 Esc 键 -> 返回菜单" << endl;
            cout << string(50, '=') << endl;
            runMonitor(cfg, recognizer);
            // 清空控制台输入缓冲区（监控模式下按的C/V/Esc会残留，导致菜单输入异常）
            FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
        } else if (c == '2') {
            cout << "\n切换校准方案:" << endl;
            for (int i = 0; i < 4; i++) {
                string status = cfg.profiles[i].initialized ? "已设置" : "默认";
                string marker = (i == cfg.currentProfile) ? " <-- 当前" : "";
                cout << "  " << (i + 1) << ". 方案" << (i + 1) << " (" << status << ")" << marker << endl;
            }
            cout << "输入方案编号 (1-4，回车取消): ";
            string s;
            getline(cin, s);
            if (!s.empty()) {
                try {
                    int idx = stoi(s) - 1;
                    if (idx >= 0 && idx < 4) {
                        switchProfile(cfg, idx);
                        saveConfig(configPath, cfg);
                        cout << "已切换到方案" << (idx + 1) << "！" << endl;
                        cout << "牌位置: ";
                        for (auto& p : cfg.cardCenters) cout << "(" << p.x << "," << p.y << ") ";
                        cout << endl;
                    } else {
                        cout << "无效编号" << endl;
                    }
                } catch (...) { cout << "输入无效" << endl; }
            }
        } else if (c == '3') {
            cout << "\n修改当前方案（方案" << (cfg.currentProfile + 1) << "）：拖拽红框(牌)和绿框(运算符)，滚轮调牌大小，Enter保存，Esc取消" << endl;
            bool ok = runCalibration(cfg);
            if (ok) {
                syncActiveToProfile(cfg);
                saveConfig(configPath, cfg);
                cout << "方案" << (cfg.currentProfile + 1) << "已保存！" << endl;
                cout << "牌位置: ";
                for (auto& p : cfg.cardCenters) cout << "(" << p.x << "," << p.y << ") ";
                cout << endl;
                cout << "裁剪尺寸: " << cfg.cardCrop.width << "x" << cfg.cardCrop.height << endl;
            } else {
                cout << "已取消校准" << endl;
            }
        } else if (c == '4') {
            cout << "\n当前设置：" << endl;
            cout << "  1.识别->执行延迟: " << cfg.recognizeDelayMs << "ms" << endl;
            cout << "  2.移动后等待: " << cfg.moveDelayMs << "ms" << endl;
            cout << "  3.按下保持: " << cfg.mouseDownMs << "ms" << endl;
            cout << "输入新的识别->执行延迟(ms，回车保持" << cfg.recognizeDelayMs << "): ";
            string s1;
            getline(cin, s1);
            if (!s1.empty()) {
                try { cfg.recognizeDelayMs = max(0, stoi(s1)); } catch (...) {}
            }
            cout << "输入新的移动后等待(ms，回车保持" << cfg.moveDelayMs << "): ";
            string s2;
            getline(cin, s2);
            if (!s2.empty()) {
                try { cfg.moveDelayMs = max(0, stoi(s2)); } catch (...) {}
            }
            cout << "输入新的按下保持时间(ms，回车保持" << cfg.mouseDownMs << "): ";
            string s3;
            getline(cin, s3);
            if (!s3.empty()) {
                try { cfg.mouseDownMs = max(0, stoi(s3)); } catch (...) {}
            }
            g_moveDelayMs = cfg.moveDelayMs;
            g_mouseDownMs = cfg.mouseDownMs;
            saveConfig(configPath, cfg);
            cout << "已保存！识别延迟=" << cfg.recognizeDelayMs << "ms, 移动等待=" << cfg.moveDelayMs << "ms, 按下保持=" << cfg.mouseDownMs << "ms" << endl;
        } else if (c == '5') {
            while (true) {
                cout << "\n自动切题：" << (cfg.autoAdvance ? "【已开启】" : "【已关闭】") << endl;
                cout << "  每次答题序列执行完后，按 1→2→3→4→5→1 循环追加点击一个切题点" << endl;
                cout << "  （追加点击的移动等待/按下保持与答题序列相同，在4.延迟设置中调整；V/Esc后重置为第1点）" << endl;
                cout << "  当前切题点:";
                for (int i = 0; i < 5; i++)
                    cout << " " << (i + 1) << "(" << cfg.advancePoints[i].x << "," << cfg.advancePoints[i].y << ")";
                cout << endl;
                cout << "  1. 开启/关闭" << endl;
                cout << "  2. 可视化设置5个切题点" << endl;
                cout << "  0. 返回主菜单" << endl;
                cout << "选择 (0-2): ";
                string ac;
                getline(cin, ac);
                if (ac.empty()) continue;
                char a = ac[0];
                if (a == '1') {
                    cfg.autoAdvance = !cfg.autoAdvance;
                    saveConfig(configPath, cfg);
                    cout << "自动切题已" << (cfg.autoAdvance ? "开启" : "关闭") << "并保存" << endl;
                } else if (a == '2') {
                    cout << "\n进入全屏设置：拖动编号1-5的绿框到目标位置，滚轮无效，Enter保存，Esc取消" << endl;
                    bool ok = runAdvanceCalibration(cfg.advancePoints);
                    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
                    if (ok) {
                        saveConfig(configPath, cfg);
                        cout << "切题点已保存：";
                        for (int i = 0; i < 5; i++)
                            cout << (i + 1) << "(" << cfg.advancePoints[i].x << "," << cfg.advancePoints[i].y << ") ";
                        cout << endl;
                    } else {
                        cout << "已取消，切题点未改动" << endl;
                    }
                } else if (a == '0') {
                    break;
                }
            }
        } else if (c == '0') {
            cout << "退出程序" << endl;
            break;
        }
    }

    timeEndPeriod(1);
    return 0;
    } catch (const exception& e) {
        cerr << "\n[崩溃] 异常: " << e.what() << endl;
        system("pause");
        return 1;
    } catch (...) {
        cerr << "\n[崩溃] 未知异常" << endl;
        system("pause");
        return 1;
    }
}
