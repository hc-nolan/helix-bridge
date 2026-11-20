/* winshot.exe <title-substring> <out.bmp>: capture a top-level window (inside Wine) to BMP. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static const char *g_needle;
static HWND g_found;

static BOOL CALLBACK find(HWND h, LPARAM l)
{
    (void)l;
    char t[256];
    if (IsWindowVisible(h) && GetWindowTextA(h, t, sizeof t) && strstr(t, g_needle)) { g_found = h; return FALSE; }
    return TRUE;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: winshot <title> <out.bmp>\n"); return 2; }
    g_needle = argv[1];
    EnumWindows(find, 0);
    if (!g_found) { fprintf(stderr, "window not found\n"); return 1; }
    RECT r; GetWindowRect(g_found, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC wdc = GetWindowDC(g_found), mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    SelectObject(mdc, bmp);
    if (!PrintWindow(g_found, mdc, 2 /* PW_RENDERFULLCONTENT */)) BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY);
    BITMAPINFOHEADER bi = { sizeof bi, w, -h, 1, 24, BI_RGB };
    int stride = (w * 3 + 3) & ~3;
    DWORD sz = stride * h;
    void *px = HeapAlloc(GetProcessHeap(), 0, sz);
    GetDIBits(mdc, bmp, 0, h, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh = { 0x4d42, sizeof fh + sizeof bi + sz, 0, 0, sizeof fh + sizeof bi };
    FILE *f = fopen(argv[2], "wb");
    if (!f) return 1;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&bi, sizeof bi, 1, f); fwrite(px, sz, 1, f); fclose(f);
    printf("%dx%d\n", w, h);
    return 0;
}
