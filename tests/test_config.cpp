// 自动切题功能验证（不点击、不监控）：
//  1) 现有配置加载：切题点补齐为5个
//  2) JSON往返：开关+坐标 保存后再加载完全一致
//  3) 旧配置兼容：无auto_advance字段时默认关闭+5默认点，延迟字段正常
//  4) 可视化设置窗口：Enter=保存返回true；Esc=取消返回false且坐标还原
#define main disabled_24point_main
#include "../auto_24point.cpp"
#undef main

#include <thread>

static int g_fail = 0;
void check(bool cond, const char* name) {
    cout << (cond ? "[PASS] " : "[FAIL] ") << name << endl;
    if (!cond) g_fail++;
}

int main() {
    // 1. 加载现有配置
    Config c = loadConfig("24point_config.json");
    check((int)c.advancePoints.size() == 5, "现有配置加载后切题点为5个");
    cout << "  enabled=" << c.autoAdvance << " points:";
    for (auto& p : c.advancePoints) cout << " (" << p.x << "," << p.y << ")";
    cout << endl;

    // 2. JSON往返
    c.autoAdvance = true;
    c.advancePoints[2] = Point(1234, 567);
    Point keep0 = c.advancePoints[0], keep4 = c.advancePoints[4];
    saveConfig("tests\\cfg_rt.json", c);
    Config c2 = loadConfig("tests\\cfg_rt.json");
    check(c2.autoAdvance == true, "往返后开关=true");
    check((int)c2.advancePoints.size() == 5, "往返后仍为5点");
    check(c2.advancePoints[2] == Point(1234, 567), "往返后修改点坐标一致");
    check(c2.advancePoints[0] == keep0 && c2.advancePoints[4] == keep4, "往返后其余点保持");

    // 3. 旧配置兼容（无auto_advance字段）
    {
        ofstream o("tests\\cfg_old.json");
        o << "{\"recognize_delay_ms\":7,\"move_delay_ms\":8,\"mouse_down_ms\":9}";
        o.close();
    }
    Config c3 = loadConfig("tests\\cfg_old.json");
    check(c3.autoAdvance == false, "旧配置默认关闭");
    check((int)c3.advancePoints.size() == 5, "旧配置补5个默认点");
    check(c3.recognizeDelayMs == 7 && c3.moveDelayMs == 8 && c3.mouseDownMs == 9, "旧配置延迟字段正常");

    // 4. 可视化窗口：Enter保存
    {
        vector<Point> pts = defaultAdvancePoints();
        Point before = pts[0];
        thread t([]() {
            Sleep(500);
            HWND h = FindWindowA("24PointAdvanceOverlay", NULL);
            if (h) PostMessageA(h, WM_KEYDOWN, VK_RETURN, 0);
        });
        bool ok = runAdvanceCalibration(pts);
        t.join();
        check(ok == true, "设置窗口Enter返回true(保存)");
        check((int)pts.size() == 5 && pts[0] == before, "保存后点数/坐标不变");
    }

    // 5. 可视化窗口：Esc取消且还原（测试线程先把点2改到(11,22)，Esc后应恢复）
    {
        vector<Point> pts = defaultAdvancePoints();
        Point orig1 = pts[1];
        thread t([&]() {
            Sleep(500);
            HWND h = FindWindowA("24PointAdvanceOverlay", NULL);
            if (h && g_advPoints) g_advPoints->at(1) = Point(11, 22); // 模拟拖动改动
            if (h) PostMessageA(h, WM_KEYDOWN, VK_ESCAPE, 0);
        });
        bool ok = runAdvanceCalibration(pts);
        t.join();
        check(ok == false, "设置窗口Esc返回false(取消)");
        check(pts[1] == orig1 && pts[1] != Point(11, 22), "取消后坐标还原为进入时");
    }

    cout << (g_fail == 0 ? "=== 全部通过 ===" : "=== 存在失败 ===") << endl;
    return g_fail ? 1 : 0;
}
