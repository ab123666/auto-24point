// DXGI区域截图验证：初始化桌面复制，用新的CopySubresourceRegion路径截图并存盘，
// 人工/程序检查：尺寸正确、无黑帧、无错位、RowPitch处理正确、区域复用与重建正常。
#define main disabled_24point_main
#include "../auto_24point.cpp"
#undef main

int main() {
    try {
        if (!g_dxgi.init()) { cerr << "DXGI init failed" << endl; return 1; }
        cout << "DXGI ok, screen " << g_dxgi.screenW << "x" << g_dxgi.screenH << endl;

        // 区域1：左上角小区域（首次创建staging）
        Mat a = g_dxgi.captureRegion(100, 100, 400, 300);
        if (a.empty()) { cerr << "capture a empty" << endl; return 2; }
        // 同尺寸再截一次（复用staging）
        Mat a2 = g_dxgi.captureRegion(100, 100, 400, 300);
        if (a2.empty()) { cerr << "capture a2 empty" << endl; return 2; }
        // 区域2：不同尺寸（触发staging重建），对应配置profile0的4张牌包围盒+padding
        Mat b = g_dxgi.captureRegion(1471, 594, 497, 599);
        if (b.empty()) { cerr << "capture b empty" << endl; return 2; }
        // 边界：超出屏幕的宽高应被clamp而非失败
        Mat c = g_dxgi.captureRegion(2800, 1700, 300, 300);
        if (c.empty()) { cerr << "capture c(edge) empty" << endl; return 2; }

        // 交叉验证：同区域用独立的GDI路径截图，若同样全黑说明屏幕本身就是黑的
        Mat gdiA = captureRegion(100, 100, 400, 300); // 全局GDI函数，非DXGI方法
        imwrite("tests\\capture_gdi_a.png", gdiA);
        Mat gdiGray; cvtColor(gdiA, gdiGray, COLOR_BGR2GRAY);
        cout << "gdi a nonblack=" << (double)countNonZero(gdiGray > 10) / (gdiA.rows * gdiA.cols) << endl;

        Mat aBgr, bBgr, cBgr;
        cvtColor(a, aBgr, COLOR_BGRA2BGR);
        cvtColor(b, bBgr, COLOR_BGRA2BGR);
        cvtColor(c, cBgr, COLOR_BGRA2BGR);
        imwrite("tests\\capture_a.png", aBgr);
        imwrite("tests\\capture_b_cards.png", bBgr);
        imwrite("tests\\capture_c_edge.png", cBgr);

        // 非黑像素比例（全黑通常意味着拷贝/坐标错误）
        auto nonBlackRatio = [](const Mat& m) {
            Mat g; cvtColor(m, g, COLOR_BGRA2GRAY);
            return (double)countNonZero(g > 10) / (m.rows * m.cols);
        };
        cout << "a: " << a.cols << "x" << a.rows << " ch=" << a.channels()
             << " nonblack=" << nonBlackRatio(a) << endl;
        cout << "b: " << b.cols << "x" << b.rows << " ch=" << b.channels()
             << " nonblack=" << nonBlackRatio(b) << endl;
        cout << "c: " << c.cols << "x" << c.rows << " ch=" << c.channels()
             << " nonblack=" << nonBlackRatio(c) << endl;

        // 连续截图计时（区域拷贝路径）：只统计成功帧
        {
            LARGE_INTEGER qf, qa, qb;
            QueryPerformanceFrequency(&qf);
            const int N = 300;
            int ok = 0;
            QueryPerformanceCounter(&qa);
            for (int i = 0; i < N; i++) {
                Mat f = g_dxgi.captureRegion(1471, 594, 497, 599);
                if (!f.empty()) ok++;
            }
            QueryPerformanceCounter(&qb);
            double total = (double)(qb.QuadPart - qa.QuadPart) * 1000.0 / qf.QuadPart;
            cout << "区域截图: " << ok << "/" << N << " 帧成功, 总耗时 " << fixed << setprecision(1)
                 << total << "ms, 含等待平均 " << total / N << "ms/次" << endl;
        }

        g_dxgi.release();
        cout << "capture test done" << endl;
        return 0;
    } catch (const exception& e) {
        cerr << "异常: " << e.what() << endl;
        return 3;
    }
}
