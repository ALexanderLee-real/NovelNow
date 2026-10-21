/* host_webview.c - WebView2 COM callbacks + screenshots + WndProc + WinMain (split from host.c 2026-10-18, zero behavior change) */
#include "host_internal.h"


/* ============================================================
 * 17. WebView2 COM 回调（手写 vtable，保持纯 C）
 * ============================================================ */

#define DEFINE_CB(NAME, IFACE)                                                  \
    typedef struct NAME { IFACE iface; LONG ref; } NAME;                        \
    static HRESULT STDMETHODCALLTYPE NAME##_QI(IFACE *This, REFIID riid, void **ppv) \
    {                                                                           \
        if (!ppv) return E_POINTER;                                             \
        if (IsEqualIID(riid, &IID_IUnknown) ||                                  \
            IsEqualIID(riid, &IID_##IFACE)) {                                   \
            *ppv = This; This->lpVtbl->AddRef(This); return S_OK;               \
        }                                                                       \
        *ppv = NULL; return E_NOINTERFACE;                                      \
    }                                                                           \
    static ULONG STDMETHODCALLTYPE NAME##_AddRef(IFACE *This)                   \
    { NAME *s = (NAME *)This; return (ULONG)InterlockedIncrement(&s->ref); }    \
    static ULONG STDMETHODCALLTYPE NAME##_Release(IFACE *This)                  \
    {                                                                           \
        NAME *s = (NAME *)This;                                                 \
        LONG r = InterlockedDecrement(&s->ref);                                 \
        if (r == 0) free(s);                                                    \
        return (ULONG)r;                                                        \
    }

/* ---- 环境创建完成 ---- */
HRESULT STDMETHODCALLTYPE CtrlCB_QI(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, REFIID riid, void **ppv);
ULONG   STDMETHODCALLTYPE CtrlCB_AddRef(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This);
ULONG   STDMETHODCALLTYPE CtrlCB_Release(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This);
HRESULT STDMETHODCALLTYPE CtrlCB_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT errorCode, ICoreWebView2Controller *controller);

typedef struct CtrlCB { ICoreWebView2CreateCoreWebView2ControllerCompletedHandler iface; LONG ref; } CtrlCB;

HRESULT STDMETHODCALLTYPE CtrlCB_QI(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CtrlCB_AddRef(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This)
{
    CtrlCB *s = (CtrlCB *)This;
    return (ULONG)InterlockedIncrement(&s->ref);
}

ULONG STDMETHODCALLTYPE CtrlCB_Release(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This)
{
    CtrlCB *s = (CtrlCB *)This;
    LONG r = InterlockedDecrement(&s->ref);
    if (r == 0) free(s);
    return (ULONG)r;
}

ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_CtrlCBVtbl = {
    CtrlCB_QI, CtrlCB_AddRef, CtrlCB_Release, CtrlCB_Invoke
};

DEFINE_CB(EnvCB, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)

HRESULT STDMETHODCALLTYPE EnvCB_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Environment *env)
{
    CtrlCB *cb;
    (void)This;
    if (FAILED(errorCode) || !env) {
        log_line("!! 环境创建失败 hr=0x%08lX（WebView2 Runtime 可能未安装）", (unsigned long)errorCode);
        if (g_hwnd) {
            MessageBoxW(g_hwnd,
                L"WebView2 Runtime 初始化失败。\n\n"
                L"本程序需要 Microsoft Edge WebView2 Runtime（Win11 自带）。\n"
                L"请访问 https://developer.microsoft.com/microsoft-edge/webview2/ 安装后重试。",
                L"novelnow", MB_OK | MB_ICONERROR);
        }
        return S_OK;
    }
    log_line("OK  环境创建成功");

    cb = (CtrlCB *)calloc(1, sizeof(CtrlCB));
    if (!cb) return S_OK;
    cb->iface.lpVtbl = &g_CtrlCBVtbl;
    cb->ref = 1;
    if (FAILED(env->lpVtbl->CreateCoreWebView2Controller(
            env, g_hwnd, (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)cb)))
        log_line("!! CreateCoreWebView2Controller 调用失败");
    return S_OK;
}

ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_EnvCBVtbl = {
    EnvCB_QI, EnvCB_AddRef, EnvCB_Release, EnvCB_Invoke
};

/* ---- JS -> C 消息 ---- */
DEFINE_CB(MsgCB, ICoreWebView2WebMessageReceivedEventHandler)

HRESULT STDMETHODCALLTYPE MsgCB_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args)
{
    LPWSTR raw = NULL;
    char *utf8;
    cJSON *root;
    (void)This; (void)sender;

    if (!args) return S_OK;
    if (FAILED(args->lpVtbl->TryGetWebMessageAsString(args, &raw)) || !raw) return S_OK;

    utf8 = w_to_u8_alloc(raw);
    CoTaskMemFree(raw);
    if (!utf8) return S_OK;

    /* 消息可能很大（整章正文），栈上放不下，全部堆分配 */
    root = cJSON_Parse(utf8);
    if (root) {
        const char *c = j_str(root, "cmd", "?");
        log_line("<== JS cmd=%s (%d 字节)", c, (int)strlen(utf8));
        /* 首条消息证明「页面已加载 + 桥接已通」，比 NavigationCompleted 可靠：
           NavigateToString 的文档 URL 恒为 about:blank，导航完成事件也可能不来。 */
        if (!g.webview_ready) {
            g.webview_ready = 1;
            log_line("页面就绪（收到首条 JS 消息）");
            /* 可见性自愈不在这里启动——它已提前到 controller 创建时无条件运行，
               否则握手回执会撞在 IsVisible=FALSE 的窗口里被丢掉。 */
        }
        cmd_dispatch(root);
        cJSON_Delete(root);
    } else {
        log_line("!! 无法解析 JS 消息（前 200 字节）: %.200s", utf8);
    }
    free(utf8);
    return S_OK;
}

ICoreWebView2WebMessageReceivedEventHandlerVtbl g_MsgCBVtbl = {
    MsgCB_QI, MsgCB_AddRef, MsgCB_Release, MsgCB_Invoke
};

/* ---- 截图（供自动化验收，--shot 使用） ---- */
DEFINE_CB(CdpCB, ICoreWebView2CallDevToolsProtocolMethodCompletedHandler)

int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

unsigned char *b64_decode(const char *in, int *outLen)
{
    int len = (int)strlen(in), i, o = 0, q[4], qn = 0;
    unsigned char *out = (unsigned char *)malloc((size_t)(len / 4 * 3 + 8));
    if (!out) return NULL;
    for (i = 0; i < len; i++) {
        int v;
        if (in[i] == '=' || in[i] == '"') break;
        v = b64_val(in[i]);
        if (v < 0) continue;
        q[qn++] = v;
        if (qn == 4) {
            out[o++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
            out[o++] = (unsigned char)(((q[1] & 15) << 4) | (q[2] >> 2));
            out[o++] = (unsigned char)(((q[2] & 3) << 6) | q[3]);
            qn = 0;
        }
    }
    if (qn == 2) out[o++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
    else if (qn == 3) {
        out[o++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
        out[o++] = (unsigned char)(((q[1] & 15) << 4) | (q[2] >> 2));
    }
    *outLen = o;
    return out;
}

static HRESULT cdp_invoke_body(
    ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *This,
    HRESULT errorCode, LPCWSTR jsonResult)
{
    char *utf8;
    const char *p, *q;
    int n;
    unsigned char *png;
    int pngLen = 0;
    char path[2048];
    FILE *f;
    (void)This;

    if (FAILED(errorCode) || !jsonResult) {
        log_line("!! CDP 截图失败 hr=0x%08lX", (unsigned long)errorCode);
        if (g_shotMode) SetTimer(g_hwnd, 99, 800, NULL);
        return S_OK;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, jsonResult, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return S_OK;
    utf8 = (char *)malloc((size_t)n);
    if (!utf8) return S_OK;
    WideCharToMultiByte(CP_UTF8, 0, jsonResult, -1, utf8, n, NULL, NULL);

    p = strstr(utf8, "\"data\":\"");
    if (!p) { free(utf8); return S_OK; }
    p += 8;
    q = strchr(p, '"');
    if (!q) { free(utf8); return S_OK; }
    {
        char *b64 = (char *)malloc((size_t)(q - p) + 1);
        if (!b64) { free(utf8); return S_OK; }
        memcpy(b64, p, (size_t)(q - p));
        b64[q - p] = '\0';
        png = b64_decode(b64, &pngLen);
        free(b64);
    }
    free(utf8);

    if (png && pngLen > 0) {
        snprintf(path, sizeof(path), "%ls\\screenshot.png", g_exeDir);
        f = fopen(path, "wb");
        if (f) {
            fwrite(png, 1, (size_t)pngLen, f);
            fclose(f);
            log_line("OK  已截图 screenshot.png (%d 字节)", pngLen);
        }
    }
    free(png);
    if (g_shotMode) SetTimer(g_hwnd, 99, 900, NULL);
    return S_OK;
}

/* 一发式 completion handler 的自我释放约定：Invoke 恰好被调用一次，调完把
   我们自己那份引用还掉（API 若另持一份，它自己会还）。
   忘还=泄漏（无害）；还两次=UAF（致命）——只在 wrapper 里还这一次，
   函数体的所有 return 都不碰引用计数。 */
HRESULT STDMETHODCALLTYPE CdpCB_Invoke(
    ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *This,
    HRESULT errorCode, LPCWSTR jsonResult)
{
    HRESULT r = cdp_invoke_body(This, errorCode, jsonResult);
    This->lpVtbl->Release(This);
    return r;
}

ICoreWebView2CallDevToolsProtocolMethodCompletedHandlerVtbl g_CdpCBVtbl = {
    CdpCB_QI, CdpCB_AddRef, CdpCB_Release, CdpCB_Invoke
};

/* 把 PNG 字节落盘到 exe 同目录 */
void write_png_bytes(const void *data, size_t len)
{
    char path[2048];
    FILE *f;
    if (!data || len == 0) { log_line("!! 截图数据为空"); return; }
    snprintf(path, sizeof(path), "%ls\\screenshot.png", g_exeDir);
    f = fopen(path, "wb");
    if (!f) { log_line("!! 无法写入 screenshot.png"); return; }
    fwrite(data, 1, len, f);
    fclose(f);
    log_line("OK  已截图 screenshot.png (%d 字节)", (int)len);
}

/* 备用路径：CDP Page.captureScreenshot（需要 Runtime 正常派发 CDP 回调） */
void request_screenshot_cdp(void)
{
    CdpCB *cb;
    HRESULT hr;
    log_line("请求截图…（CDP 备用路径）");
    if (!g_wv) { log_line("!! g_wv 为空，无法截图"); return; }
    cb = (CdpCB *)calloc(1, sizeof(CdpCB));
    if (!cb) return;
    cb->iface.lpVtbl = &g_CdpCBVtbl;
    cb->ref = 1;   /* 正规 COM 计数（P1：曾 ref=1000000 诊断泄漏——「回调不来」
                      早已破案是可见性问题，与 handler 生命周期无关） */
    hr = g_wv->lpVtbl->CallDevToolsProtocolMethod(
        g_wv, L"Page.captureScreenshot", L"{\"format\":\"png\"}",
        (ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *)cb);
    if (FAILED(hr))
        cb->iface.lpVtbl->Release(&cb->iface);   /* 调用失败 API 不接手也不会回调，我们这份自己还掉 */
    log_line("CallDevToolsProtocolMethod hr=0x%08lX", (unsigned long)hr);
}

/* ---- 主路径：ICoreWebView2::CapturePreview ---- */
DEFINE_CB(CapCB, ICoreWebView2CapturePreviewCompletedHandler)

/* 诊断用：验证 completion handler 机制本身是否可用 */
DEFINE_CB(EsCB, ICoreWebView2ExecuteScriptCompletedHandler)

HRESULT STDMETHODCALLTYPE EsCB_Invoke(
    ICoreWebView2ExecuteScriptCompletedHandler *This,
    HRESULT errorCode, LPCWSTR resultJson)
{
    (void)This;
    log_line("ExecuteScript 回调 hr=0x%08lX 结果=%ls",
             (unsigned long)errorCode, resultJson ? resultJson : L"(null)");
    /* 自我释放约定同 CdpCB_Invoke */
    This->lpVtbl->Release(This);
    return S_OK;
}

ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_EsCBVtbl = {
    EsCB_QI, EsCB_AddRef, EsCB_Release, EsCB_Invoke
};

void test_execute_script(void)
{
    EsCB *cb;
    HRESULT hr;
    if (!g_wv) return;
    cb = (EsCB *)calloc(1, sizeof(EsCB));
    if (!cb) return;
    cb->iface.lpVtbl = &g_EsCBVtbl;
    cb->ref = 1;
    hr = g_wv->lpVtbl->ExecuteScript(
        g_wv, L"1+1", (ICoreWebView2ExecuteScriptCompletedHandler *)cb);
    if (FAILED(hr))
        cb->iface.lpVtbl->Release(&cb->iface);   /* 同 CdpCB：失败路径自己还 */
    log_line("ExecuteScript hr=0x%08lX", (unsigned long)hr);
}

static HRESULT cap_invoke_body(
    ICoreWebView2CapturePreviewCompletedHandler *This, HRESULT errorCode)
{
    STATSTG st;
    LARGE_INTEGER zero;
    unsigned char *buf;
    ULONG rd = 0;
    size_t n;
    (void)This;

    log_line("CapCB_Invoke 被调用 hr=0x%08lX", (unsigned long)errorCode);

    if (FAILED(errorCode) || !g_shotStream) {
        log_line("!! CapturePreview 失败 hr=0x%08lX", (unsigned long)errorCode);
        request_screenshot_cdp();
        return S_OK;
    }
    memset(&st, 0, sizeof(st));
    if (FAILED(g_shotStream->lpVtbl->Stat(g_shotStream, &st, STATFLAG_NONAME))) {
        log_line("!! 无法读取截图流大小");
        return S_OK;
    }
    n = (size_t)st.cbSize.QuadPart;
    if (n == 0) { log_line("!! 截图流为空"); return S_OK; }

    zero.QuadPart = 0;
    g_shotStream->lpVtbl->Seek(g_shotStream, zero, STREAM_SEEK_SET, NULL);
    buf = (unsigned char *)malloc(n);
    if (!buf) return S_OK;
    if (SUCCEEDED(g_shotStream->lpVtbl->Read(g_shotStream, buf, (ULONG)n, &rd)) && rd > 0)
        write_png_bytes(buf, (size_t)rd);
    else
        log_line("!! 读取截图流失败");
    free(buf);
    return S_OK;
}

/* 自我释放约定同 CdpCB_Invoke：一发式 handler，Invoke 后放掉我们的引用 */
HRESULT STDMETHODCALLTYPE CapCB_Invoke(
    ICoreWebView2CapturePreviewCompletedHandler *This, HRESULT errorCode)
{
    HRESULT r = cap_invoke_body(This, errorCode);
    This->lpVtbl->Release(This);
    return r;
}

ICoreWebView2CapturePreviewCompletedHandlerVtbl g_CapCBVtbl = {
    CapCB_QI, CapCB_AddRef, CapCB_Release, CapCB_Invoke
};

void request_screenshot(void)
{
    CapCB *cb;
    HGLOBAL hg;
    HRESULT hr;

    log_line("请求截图…");
    if (!g_wv) { log_line("!! g_wv 为空，无法截图"); return; }

    /* 隐藏窗口下 WebView2 合成器不产帧，CapturePreview/CDP 都会返回
       ERROR_INVALID_STATE(0x8007139F)。截图前重申一次可见性与尺寸。 */
    if (g_ctrl) {
        RECT rc;
        BOOL vis = FALSE;
        HRESULT hb, hv, hg2;   /* 原名 hg 遮蔽外层 HGLOBAL hg（P1-⑳），改名 */
        GetClientRect(g_hwnd, &rc);
        log_line("截图前 bounds=(%ld,%ld,%ld,%ld) 窗口可见=%d 最小化=%d",
                 (long)rc.left, (long)rc.top, (long)rc.right, (long)rc.bottom,
                 IsWindowVisible(g_hwnd) ? 1 : 0, IsIconic(g_hwnd) ? 1 : 0);
        hb = g_ctrl->lpVtbl->put_Bounds(g_ctrl, rc);
        hv = g_ctrl->lpVtbl->put_IsVisible(g_ctrl, TRUE);
        hg2 = g_ctrl->lpVtbl->get_IsVisible(g_ctrl, &vis);
        log_line("put_Bounds hr=0x%08lX put_IsVisible hr=0x%08lX get_IsVisible hr=0x%08lX vis=%d",
                 (unsigned long)hb, (unsigned long)hv, (unsigned long)hg2, vis ? 1 : 0);
    }

    if (g_shotStream) { g_shotStream->lpVtbl->Release(g_shotStream); g_shotStream = NULL; }
    hg = GlobalAlloc(GMEM_MOVEABLE, 0);
    if (!hg) { log_line("!! GlobalAlloc 失败"); return; }
    hr = CreateStreamOnHGlobal(hg, TRUE, &g_shotStream);
    if (FAILED(hr) || !g_shotStream) {
        GlobalFree(hg);
        log_line("!! CreateStreamOnHGlobal hr=0x%08lX", (unsigned long)hr);
        return;
    }

    cb = (CapCB *)calloc(1, sizeof(CapCB));
    if (!cb) return;
    cb->iface.lpVtbl = &g_CapCBVtbl;
    cb->ref = 1;   /* 同 CdpCB，正规计数 */
    hr = g_wv->lpVtbl->CapturePreview(
        g_wv, COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, g_shotStream,
        (ICoreWebView2CapturePreviewCompletedHandler *)cb);
    if (FAILED(hr))
        cb->iface.lpVtbl->Release(&cb->iface);   /* 失败路径自己还 */
    log_line("CapturePreview hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) request_screenshot_cdp();

    /* 诊断：completion handler 机制是否可用（若此回调也不来，说明是
       Runtime 侧不在可回调状态，而非本文件的 QI/vtable 问题）。 */
    test_execute_script();
}

/* ---- 导航完成 ---- */
DEFINE_CB(NavCB, ICoreWebView2NavigationCompletedEventHandler)

HRESULT STDMETHODCALLTYPE NavCB_Invoke(
    ICoreWebView2NavigationCompletedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args)
{
    BOOL ok = FALSE;
    (void)This; (void)sender;
    if (!args) return S_OK;
    args->lpVtbl->get_IsSuccess(args, &ok);
    log_line("导航完成 success=%d", ok ? 1 : 0);
    if (ok) g.webview_ready = 1;   /* 失败不置位，留给兜底重试（P1-⑱） */

    return S_OK;
}

ICoreWebView2NavigationCompletedEventHandlerVtbl g_NavCBVtbl = {
    NavCB_QI, NavCB_AddRef, NavCB_Release, NavCB_Invoke
};

/* 读取 ui 目录下的文本文件（UTF-8，去 BOM），失败返回 NULL */
char *read_ui_file(const char *name)
{
    WCHAR path[MAX_PATH];
    HANDLE hf;
    DWORD size = 0, rd = 0;
    char *raw;
    WCHAR wname[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, MAX_PATH);
    swprintf(path, MAX_PATH, L"%s\\ui\\%s", g_exeDir, wname);

    hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(hf, NULL);
    if (size == 0 || size == INVALID_FILE_SIZE) { CloseHandle(hf); return NULL; }
    raw = (char *)malloc(size + 1);
    if (!raw) { CloseHandle(hf); return NULL; }
    if (!ReadFile(hf, raw, size, &rd, NULL)) {
        CloseHandle(hf); free(raw); return NULL;   /* 读失败返 NULL，不能返空串冒充成功（P1-⑰） */
    }
    CloseHandle(hf);
    raw[rd] = '\0';
    return raw;
}

/* 把 index.html 的外链 css/js 内联成一份自包含页面。
   虚拟主机映射不可用时用它兜底（NavigateToString 的 base 是 about:blank，
   相对路径取不到资源）。 */
void navigate_inline(void)
{
    char *html = read_ui_file("index.html");
    strbuf_t *buf;
    char *out;
    WCHAR *wide;
    /* 通用外链内联：按出现顺序遍历 <link rel="stylesheet" href="X"> 与
       <script src="X"></script>，逐个读 ui/X 内嵌。UI 已按功能拆成 7 个
       js 文件（2026-10-18），文件名不再写死，新增文件自动被内联。 */
    const char *link_prefix = "<link rel=\"stylesheet\" href=\"";
    const char *script_prefix = "<script src=\"";
    if (!html) {
        log_line("!! 读不到 ui/index.html，无法内联");
        return;
    }
    buf = strbuf_create(strlen(html) + 262144);
    if (!buf) { free(html); return; }
    {
        const char *p = html;
        for (;;) {
            const char *a = strstr(p, link_prefix);
            const char *b = strstr(p, script_prefix);
            const char *first = NULL;
            int which = 0;
            if (a && (!b || a < b)) { first = a; which = 1; }
            else if (b)             { first = b; which = 2; }
            if (!first) break;
            strbuf_append_len(buf, p, (size_t)(first - p));
            {
                const char *ns = first + strlen((which == 1) ? link_prefix : script_prefix);
                const char *ne = strchr(ns, '"');
                char name[128];
                size_t nlen = ne ? (size_t)(ne - ns) : 0;
                int ok = 0;
                if (ne && nlen > 0 && nlen < sizeof(name)) {
                    memcpy(name, ns, nlen);
                    name[nlen] = '\0';
                    /* 标签闭合形态：link 以 ">" 结束，script 以 "></script>" 结束 */
                    if (which == 1) ok = (strncmp(ne + 1, ">", 1) == 0);
                    else            ok = (strncmp(ne + 1, "></script>", 10) == 0);
                    if (ok) {
                        char *content = read_ui_file(name);
                        if (which == 1) {
                            strbuf_append(buf, "<style>\n");
                            if (content) strbuf_append(buf, content);
                            strbuf_append(buf, "\n</style>");
                            p = ne + 2;
                        } else {
                            strbuf_append(buf, "<script>\n");
                            if (content) strbuf_append(buf, content);
                            strbuf_append(buf, "\n</script>");
                            p = ne + 11;
                        }
                        if (content) free(content);
                        continue;
                    }
                }
            }
            /* 形态不认识（手写变体等）：把 '<' 搬过去原样保留，继续扫描 */
            strbuf_append_len(buf, first, 1);
            p = first + 1;
        }
        strbuf_append(buf, p);
    }

    out = strbuf_dup(buf);
    strbuf_destroy(buf);
    free(html);

    wide = out ? u8_to_w(out) : NULL;
    free(out);
    if (wide) {
        HRESULT hr = g_wv->lpVtbl->NavigateToString(g_wv, wide);
        log_line("内联投递 hr=0x%08lX，HTML %d 字符",
                 (unsigned long)hr, (int)wcslen(wide));
        free(wide);
    }
}

/* file:// 直载（保留 index.html 的外链 <link>/<script>，浏览器原生加载）。
   与 NavigateToString 相比，这是「真实文档 + 真实源」，渲染管线更接近常态。 */
void navigate_file_url(void)
{
    WCHAR url[MAX_PATH + 64];
    WCHAR dir[MAX_PATH];
    int i;
    HRESULT hr;

    wcsncpy(dir, g_exeDir, MAX_PATH - 1);
    dir[MAX_PATH - 1] = L'\0';
    for (i = 0; dir[i]; i++) if (dir[i] == L'\\') dir[i] = L'/';

    swprintf(url, MAX_PATH + 64, L"file:///%s/ui/index.html", dir);
    hr = g_wv->lpVtbl->Navigate(g_wv, url);
    log_line("file:// 导航 hr=0x%08lX url=%ls", (unsigned long)hr, url);
}

/* ---- 控制器创建完成：装配页面 ---- */
/* 事件注册令牌与 handler 持有：必须存下来，WM_DESTROY 里先 remove_ 再放
   我们自己那份引用，MsgCB/NavCB 归零即释放。
   （P1：曾一个局部 token 先后接 WebMessageReceived/NavigationCompleted 两次
   add，后者覆盖前者，token 全丢——事件永远拆不掉，handler 挂到进程死） */
static EventRegistrationToken g_tokenMsg, g_tokenNav;
static MsgCB *g_msgcb;
static NavCB *g_navcb;

HRESULT STDMETHODCALLTYPE CtrlCB_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Controller *controller)
{
    RECT rc;
    ICoreWebView2Settings *settings = NULL;
    MsgCB *msgcb;
    NavCB *navcb;
    (void)This;

    if (FAILED(errorCode) || !controller) {
        log_line("!! 控制器创建失败 hr=0x%08lX", (unsigned long)errorCode);
        return S_OK;
    }
    log_line("OK  控制器创建成功");

    g_ctrl = controller;
    controller->lpVtbl->AddRef(controller);
    if (FAILED(controller->lpVtbl->get_CoreWebView2(controller, &g_wv)) || !g_wv) {
        log_line("!! get_CoreWebView2 失败");
        return S_OK;
    }
    GetClientRect(g_hwnd, &rc);
    controller->lpVtbl->put_Bounds(controller, rc);
    /* ---- 可见性前置确认（必须发生在导航之前）----
       这是本程序最要命的一个坑：
         controller 若在宿主窗口 show 之前创建，IsVisible 会被锁死在 FALSE，
         而**默认值虽然号称是 TRUE，实际读回来是 0**。
       IsVisible=FALSE 时的后果不是「画面不好看」，而是整个渲染器把页面当作不可见：
         - NavigateToString 不提交，Source 永远停在 about:blank，
           页面里的 JS 一行都不会执行；
         - C -> JS 的 PostWebMessageAsJson 返回 S_OK 却把报文丢掉；
         - JS 的 setTimeout / setInterval 全部冻结。
       症状就是「界面起来了但所有按钮都没反应」，且时好时坏
       （实测同一份程序闭环成功率只有 50%）。
       注意不能只写一次：第一次 put_IsVisible(TRUE) 反而会把状态置成 FALSE，
       要「推 + 读回」反复几次才收敛。这里在导航前先收敛好。 */
    {
        int i;
        BOOL vis = FALSE;
        for (i = 0; i < 8; i++) {
            controller->lpVtbl->put_IsVisible(controller, TRUE);
            controller->lpVtbl->get_IsVisible(controller, &vis);
            if (vis) break;
        }
        log_line("可见性前置确认：vis=%d 尝试=%d 次", vis ? 1 : 0, i + 1);
    }
    /* 兜底：导航之后仍可能被置回 FALSE，用定时器 96 继续跟直到读回 TRUE */
    g_visTries = 0;
    SetTimer(g_hwnd, 96, 100, NULL);

    if (SUCCEEDED(g_wv->lpVtbl->get_Settings(g_wv, &settings)) && settings) {
        settings->lpVtbl->put_IsStatusBarEnabled(settings, FALSE);
        settings->lpVtbl->put_AreDefaultContextMenusEnabled(settings, TRUE);
        settings->lpVtbl->put_AreDevToolsEnabled(settings, TRUE);
        settings->lpVtbl->put_IsZoomControlEnabled(settings, FALSE);
        settings->lpVtbl->Release(settings);
    }

    /* 接收 JS -> C */
    msgcb = (MsgCB *)calloc(1, sizeof(MsgCB));
    if (msgcb) {
        msgcb->iface.lpVtbl = &g_MsgCBVtbl;
        msgcb->ref = 1;   /* 我们持一份，事件源接手后另持一份（COM 约定）；
                             WM_DESTROY 里 remove_ 后才放我们的，谁最后放手谁 free */
        g_msgcb = msgcb;
        if (FAILED(g_wv->lpVtbl->add_WebMessageReceived(
                g_wv, (ICoreWebView2WebMessageReceivedEventHandler *)msgcb, &g_tokenMsg)))
            log_line("!! 注册 WebMessageReceived 失败");
        else
            log_line("OK  JS -> C 通道就绪");
    }

    /* 导航完成回调 */
    navcb = (NavCB *)calloc(1, sizeof(NavCB));
    if (navcb) {
        HRESULT hr;
        navcb->iface.lpVtbl = &g_NavCBVtbl;
        navcb->ref = 1;   /* 同 msgcb：事件源持一份，WM_DESTROY 拆除后我们再放 */
        g_navcb = navcb;
        hr = g_wv->lpVtbl->add_NavigationCompleted(
            g_wv, (ICoreWebView2NavigationCompletedEventHandler *)navcb, &g_tokenNav);
        log_line("add_NavigationCompleted hr=0x%08lX", (unsigned long)hr);
    }

    /* ---------- 加载页面 ----------
       实测结论：在本机 WebView2 Runtime 154 下，对
       `SetVirtualHostNameToFolderMapping` 注册的 https://<host>/ 调 Navigate
       会返回 S_OK 但导航永不提交（Source 一直是 about:blank），
       且随后任何 NavigateToString 都会拿到 ERROR_INVALID_STATE。
       而同环境下 NavigateToString 完全正常（spike 已验证）。
       因此默认走「读文件 -> 内联 css/js -> NavigateToString」，
       虚拟主机映射改用 --vhost 参数显式开启以便后续排查。 */
    if (g_useVHost) {
        ICoreWebView2_3 *wv3 = NULL;
        WCHAR uiDir[MAX_PATH];
        swprintf(uiDir, MAX_PATH, L"%s\\ui", g_exeDir);
        if (SUCCEEDED(g_wv->lpVtbl->QueryInterface(g_wv, &IID_ICoreWebView2_3, (void **)&wv3)) && wv3) {
            HRESULT hm = wv3->lpVtbl->SetVirtualHostNameToFolderMapping(
                    wv3, L"novelnow.local", uiDir,
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            if (SUCCEEDED(hm)) {
                HRESULT hn = g_wv->lpVtbl->Navigate(g_wv, L"https://novelnow.local/index.html");
                log_line("--vhost: 映射成功，Navigate hr=0x%08lX", (unsigned long)hn);
            } else {
                log_line("--vhost: 映射失败 hr=0x%08lX", (unsigned long)hm);
            }
            wv3->lpVtbl->Release(wv3);
        }
    } else if (g_useFileUrl) {
        navigate_file_url();
    } else {
        navigate_inline();
    }

    /* 诊断看门狗：页面若迟迟不就绪，把当前 URL 记下来便于排查 */
    SetTimer(g_hwnd, 97, 3000, NULL);
    return S_OK;
}

/* ============================================================
 * 18. 主线程消息处理（任务回执 / 询问 / 进度）
 * ============================================================ */

void handle_task_done(task_done_t *t)
{
    const char *cmd = t->cmd;

    /* ComfyUI 配图结果：不占忙闲位（没过 try_begin_task），绝不能落进下面
       通用收尾——那里的 end_task() 会把正在跑的 AI 任务的忙闲状态踩掉，
       task_done 事件也会让前端误 setBusy(false)。走专用 comfy_done 事件。 */
    if (strcmp(cmd, "comfy_test") == 0 || strcmp(cmd, "comfy_gen") == 0) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "ok", t->ok);
        add_str(d, "cmd", cmd);
        add_str(d, "message", t->message);
        if (t->payload) add_str(d, "text", t->payload);  /* {"node":..,"path":..} */
        push_event("comfy_done", d);
        push_status(t->message);
        if (!t->ok) push_toast("error", t->message);
        return;
    }

    /* 加载项目：把工作线程解析好的 project 换上 */
    if (strcmp(cmd, "load_project") == 0) {
        novel_project_t *proj = (novel_project_t *)t->ptr;
        t->ptr = NULL;   /* 所有权已由本函数接管，task_done_free 不再管它 */
        if (proj) {
            if (g.project) project_destroy(g.project);
            g.project = proj;
            g.selected_chapter = -1;
            g.dirty = 0;
            g.last_doc[0] = g.last_txt[0] = g.last_epub[0] = '\0';
        }
        end_task();
        {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddBoolToObject(d, "ok", t->ok);
            add_str(d, "cmd", cmd);
            add_str(d, "message", t->message);
            push_event("task_done", d);
        }
        push_project();
        push_project_list();
        push_status(t->message);
        if (!t->ok) push_toast("error", t->message);
        return;
    }

    /* 获取模型列表：payload 里是 {"models":[...]}（大 JSON，不能走 message） */
    if (strcmp(cmd, "fetch_models") == 0) {
        end_task();
        {
            cJSON *d = cJSON_CreateObject();
            cJSON *parsed = (t->ok && t->payload) ? cJSON_Parse(t->payload) : NULL;
            cJSON *m = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "models") : NULL;
            int  n  = (m && cJSON_IsArray(m)) ? cJSON_GetArraySize(m) : 0;
            int  ok = (t->ok && parsed && m && cJSON_IsArray(m)) ? 1 : 0;
            char msg[160];

            if (t->ok && !ok) {
                /* 以前这条路径是静默的：载荷被截断 -> cJSON_Parse 失败 -> models 缺失，
                   前端只收到 ok=true，用户看到「获取成功」却一个模型都没有。
                   现在必须留下痕迹并如实报错 */
                log_line("!! fetch_models 载荷解析失败（payload %d 字节）",
                         t->payload ? (int)strlen(t->payload) : -1);
            }
            if (ok)       snprintf(msg, sizeof(msg), "已获取 %d 个模型", n);
            else          snprintf(msg, sizeof(msg), "%s",
                                   t->message[0] ? t->message : "获取模型列表失败");

            cJSON_AddBoolToObject(d, "ok", ok);
            add_str(d, "cmd", cmd);
            add_str(d, "message", msg);
            if (ok) cJSON_AddItemToObject(d, "models", cJSON_Duplicate(m, 1));
            push_event("task_done", d);
            push_status(msg);
            if (!ok) push_toast("error", msg);
            cJSON_Delete(parsed);
        }
        return;
    }

    end_task();
    {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "ok", t->ok);
        add_str(d, "cmd", cmd);
        add_str(d, "message", t->message);
        cJSON_AddNumberToObject(d, "chapter", t->chapter);
        if (t->payload) add_str(d, "text", t->payload);  /* ai_call 等：大文本结果 */
        push_event("task_done", d);
    }
    push_status(t->message);
    if (!t->ok && t->result != ENGINE_CANCELLED) push_toast("error", t->message);
    else if (t->ok) push_toast("success", t->message);

    /* 任务改变了项目内容：推一份新快照。
       ai_call 是纯问答、不动项目，别打脏标记也别推全量快照（项目大时一次几十 KB） */
    if (strcmp(cmd, "ai_call") != 0 &&
        (t->ok || t->result == ENGINE_CANCELLED)) {
        g.dirty = 1;
        push_project();
    }
}

/* ============================================================
 * 19. 窗口过程
 * ============================================================ */

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_ctrl) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_ctrl->lpVtbl->put_Bounds(g_ctrl, rc);
        }
        return 0;

    case WM_DPICHANGED:
        if (g_ctrl) {
            RECT *r = (RECT *)lp;
            SetWindowPos(hwnd, NULL, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_GETMINMAXINFO:
        {
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            mmi->ptMinTrackSize.x = 900;
            mmi->ptMinTrackSize.y = 600;
        }
        return 0;

    case WM_SETFOCUS:
        if (g_ctrl) g_ctrl->lpVtbl->MoveFocus(g_ctrl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;

    /* ---- 延后投递 C -> JS（见 post_json_root 的说明）---- */
    case NWM_POST:
        {
            WCHAR *w = (WCHAR *)wp;
            HRESULT hr;
            if (!w) return 0;
            if (!g_wv || g_shuttingDown) { free(w); return 0; }
            /* 从 WebView2 回调里同步回投会丢包，改到这里；失败则入重投队列。 */
            hr = g_wv->lpVtbl->PostWebMessageAsJson(g_wv, w);
            if (SUCCEEDED(hr)) { free(w); return 0; }
            if (post_retry_push(w)) {
                if (g_postRetryTries == 0)
                    log_line("!! 投递失败 hr=0x%08lX，转入重投队列（当前 %d 条待发）",
                             (unsigned long)hr, g_postRetryN);
                SetTimer(hwnd, IDT_POST_RETRY, 200, NULL);
            } else {
                log_line("!! 投递失败 hr=0x%08lX 且重投队列扩容失败（内存不足），丢弃一条", (unsigned long)hr);
                free(w);
            }
        }
        return 0;

    /* ---- 任务完成 ---- */
    case NWM_TASK_DONE:
        {
            task_done_t *t = (task_done_t *)wp;
            if (t) { handle_task_done(t); task_done_free(t); }
        }
        return 0;

    /* ---- 工作线程请求用户确认：推给网页 ---- */
    case NWM_ASK:
        {
            ask_t *a = (ask_t *)wp;
            if (a) {
                /* 询问可能在本消息被处理前就已收场（用户点「取消」→ask_release_all
                   唤回、或等待超时）：再推弹窗就是「死弹窗」——推出来也没人接收
                   答复。dead=1 即等待已结束，只还消息那份引用，不推事件（P1）。 */
                if (InterlockedCompareExchange(&a->dead, 0, 0)) {
                    log_line("-- 询问 #%d 已收场，跳过迟到的弹窗", a->id);
                } else {
                    cJSON *d = cJSON_CreateObject();
                    cJSON_AddNumberToObject(d, "askId", a->id);
                    add_str(d, "text", a->text);
                    add_str(d, "kind", "chapter_review");
                    push_event("ask", d);
                }
                ask_unref(a);   /* 消息已消费，退还消息那份引用 */
            }
        }
        return 0;

    /* ---- 进度 ---- */
    case NWM_PROGRESS:
        {
            prog_t *p = (prog_t *)wp;
            if (p) {
                cJSON *d = cJSON_CreateObject();
                add_str(d, "stage", p->stage);
                cJSON_AddNumberToObject(d, "current", p->current);
                cJSON_AddNumberToObject(d, "total", p->total);
                add_str(d, "message", p->message);
                push_event("progress", d);
                if (p->message[0]) push_status(p->message);
                free(p);
            }
        }
        return 0;

    /* ---- 章节正文流式增量：推给前端实时回填 ---- */
    case NWM_CHAPTER_DELTA:
        {
            delta_t *d = (delta_t*)wp;
            if (d) {
                cJSON *j = cJSON_CreateObject();
                cJSON_AddNumberToObject(j, "index", d->index);
                add_str(j, "delta", d->text);
                push_event(d->index < 0 ? "text_delta" : "chapter_delta", j);
                free(d->text);
                free(d);
            }
        }
        return 0;

    /* ---- 逐章生成：某章完成 ----
       载荷是 ch_done_t 快照（P0-C）：绝不回读 g.project——「重写本章」被回答后
       工作线程立刻改写同一章，回读就是 UAF。push_project() 保留：本消息先于
       NWM_ASK 投递（同线程 FIFO），处理它时工作线程还阻塞在 bridge_ask，
       project 处于静止，深读安全。 */
    case NWM_CHAPTER_DONE:
        {
            ch_done_t *cd = (ch_done_t *)wp;
            if (cd) {
                cJSON *d = cJSON_CreateObject();
                cJSON_AddNumberToObject(d, "index", cd->index);
                add_str(d, "title", cd->title);
                add_str(d, "content", cd->content);
                cJSON_AddNumberToObject(d, "wordCount", cd->word_count);
                push_event("chapter_written", d);
                push_project();
                g.dirty = 1;
                free(cd->title);
                free(cd->content);
                free(cd);
            }
        }
        return 0;

    /* ---- 自动保存 ---- */
    case WM_TIMER:
        if (wp == IDT_AUTOSAVE) {
            if (g.dirty && !is_busy() && g.project && g.config) {
                if (save_project_silently()) push_status("已自动保存项目");
            }
        } else if (wp == 97) {
            /* 诊断看门狗：3 秒还未就绪则把当前 URL 记下来便于排查 */
            KillTimer(hwnd, 97);
            if (!g.webview_ready) {
                LPWSTR src = NULL;
                if (g_wv && SUCCEEDED(g_wv->lpVtbl->get_Source(g_wv, &src)) && src) {
                    char *u = w_to_u8_alloc(src);
                    log_line("!! 看门狗：导航 3 秒未完成，当前 URL=%s", u ? u : "(?)");
                    free(u);
                    CoTaskMemFree(src);
                }
            }
        } else if (wp == IDT_POST_RETRY) {
            post_retry_pump(hwnd);
        } else if (wp == 96) {
            /* 可见性自愈（现在无条件运行，见 controller 创建处的说明）。
               IsVisible 被锁死在 FALSE 时，渲染器视页面为不可见：C -> JS 报文
               被静默丢弃、JS 定时器全部冻结。这里反复重设直到读回 TRUE。
               间隔 100ms，通常 1~3 次即收敛。 */
            BOOL vis = FALSE;
            if (g_ctrl) {
                g_ctrl->lpVtbl->put_IsVisible(g_ctrl, TRUE);
                g_ctrl->lpVtbl->get_IsVisible(g_ctrl, &vis);
            }
            g_visTries++;
            if (vis || g_visTries >= 25) {
                KillTimer(hwnd, 96);
                log_line("可见性自愈结束：vis=%d 尝试=%d 次（IsIconic=%d）",
                         vis ? 1 : 0, g_visTries, IsIconic(hwnd) ? 1 : 0);
                if (g_shotMode) {
                    SetTimer(hwnd, 98, 300, NULL);
                } else if (vis) {
                    /* 确认可见之后前端才可以握手：此前发出的报文会被丢掉。
                       前端收到 view_ready 才发 hello，并有超时兜底（见 app.js）。 */
                    cJSON *d = cJSON_CreateObject();
                    push_event("view_ready", d);
                }
            }
            return 0;
        } else if (wp == 98) {
            KillTimer(hwnd, 98);
            request_screenshot();
        } else if (wp == 99) {
            KillTimer(hwnd, 99);
            if (g_holdMode) { log_line("--hold：保留窗口不退出"); return 0; }
            log_line("--shot 模式：正常退出");
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_CLOSE:
        if (is_busy()) {
            /* 不弹模态框：交给前端用自绘弹层处理，避免跨线程/模态的坑 */
            {
                cJSON *d = cJSON_CreateObject();
                add_str(d, "kind", "warn");
                add_str(d, "text", "后台任务正在进行，请等待任务完成或点击取消后再关闭窗口。");
                push_event("toast", d);
            }
            return 0;
        }
        if (g.dirty && g.project) {
            if (MessageBoxW(hwnd, L"项目有未保存的改动，是否保存后退出？",
                            L"novelnow", MB_YESNOCANCEL | MB_ICONQUESTION) == IDYES) {
                if (!save_project_silently())
                    MessageBoxW(hwnd, L"保存失败，请检查 projects 目录权限。", L"novelnow", MB_OK | MB_ICONERROR);
            }
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_shuttingDown = 1;
        KillTimer(hwnd, IDT_AUTOSAVE);
        KillTimer(hwnd, IDT_POST_RETRY);
        post_retry_clear();
        ask_release_all();
        /* 中止在途 AI 请求，让退避/限流睡眠立刻醒来，工作线程好收场 */
        ai_abort_all();   /* 关窗：AI + comfy 两条通道都掐（2026-10-19 通道上下文化后 comfy 也可掐） */
        /* 等工作线程收场（最多 3 秒），再拆共享对象（P1 竞态）。
           为什么限时：HttpSendRequest 发送阶段不可打断（g_active_req 在发送
           之后才登记），最坏卡 timeout 秒，关窗不能陪它卡。
           为什么超时后不硬拆：engine/project/config 正被工作线程改写，强行
           destroy 就是 UAF（P0-C 的写侧）。不释放只是退出时交进程回收，安全。
           comfy 线程走 launch_thread_untracked 不占计数。 */
        {
            int waited = 0;
            while (waited < 30 && InterlockedCompareExchange(&g_liveThreads, 0, 0) > 0) {
                Sleep(100);
                waited++;
            }
            if (InterlockedCompareExchange(&g_liveThreads, 0, 0) > 0) {
                log_line("!! 退出时仍有 %ld 个工作线程在跑，跳过 engine/project/config "
                         "释放（进程退出统一回收，防 UAF）",
                         (long)InterlockedCompareExchange(&g_liveThreads, 0, 0));
            } else {
                if (g.engine) { engine_destroy(g.engine); g.engine = NULL; }
                if (g.project) { project_destroy(g.project); g.project = NULL; }
                if (g.config) { config_destroy(g.config); g.config = NULL; }
            }
        }
        if (g_wv) {
            /* 拆事件注册：remove_ 会还回事件源那份引用，之后再放我们自己那份，
               MsgCB/NavCB 归零即释放（P1 COM 收尾：曾永不 remove、token 用完就丢） */
            g_wv->lpVtbl->remove_WebMessageReceived(g_wv, g_tokenMsg);
            g_wv->lpVtbl->remove_NavigationCompleted(g_wv, g_tokenNav);
            g_wv->lpVtbl->Release(g_wv);   /* get_CoreWebView2 是 AddRef 过的，不 Release 就漏一份（P1 COM 泄漏） */
            g_wv = NULL;
        }
        if (g_msgcb) { g_msgcb->iface.lpVtbl->Release(&g_msgcb->iface); g_msgcb = NULL; }
        if (g_navcb) { g_navcb->iface.lpVtbl->Release(&g_navcb->iface); g_navcb = NULL; }
        if (g_ctrl) {
            g_ctrl->lpVtbl->Close(g_ctrl);
            g_ctrl->lpVtbl->Release(g_ctrl);
            g_ctrl = NULL;
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* ============================================================
 * 20. 初始化
 * ============================================================ */

void enable_dark_titlebar(HWND hwnd)
{
    typedef HRESULT (WINAPI *PFN_DwmSet)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    BOOL dark = TRUE;
    if (!dwm) return;
    {
        PFN_DwmSet f = (PFN_DwmSet)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (f) {
            /* 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 1903+)，19 为旧版编号 */
            if (FAILED(f(hwnd, 20, &dark, sizeof(dark))))
                f(hwnd, 19, &dark, sizeof(dark));
        }
    }
    FreeLibrary(dwm);
}

void enable_dpi_awareness(void)
{
    typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    PFN_SetCtx f = u ? (PFN_SetCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;

    /* manifest 里已声明 PerMonitorV2；此时再调 API 会返回 ERROR_ACCESS_DENIED，
       属于"已生效"，不是失败。 */
    if (f && f((HANDLE)-4)) { log_line("DPI: PerMonitorV2（运行时设置）"); return; }
    if (GetLastError() == ERROR_ACCESS_DENIED) { log_line("DPI: PerMonitorV2（manifest 已声明）"); return; }
    if (SetProcessDPIAware()) log_line("DPI: SystemAware（回退）");
    else log_line("DPI: 未设置");
}

int load_loader(void)
{
    WCHAR path[MAX_PATH + 32];
    HMODULE h;
    swprintf(path, MAX_PATH + 32, L"%s\\WebView2Loader.dll", g_exeDir);
    h = LoadLibraryW(path);
    if (!h) {
        h = LoadLibraryW(L"WebView2Loader.dll");
        if (!h) { log_line("!! WebView2Loader.dll 加载失败 err=%lu", (unsigned long)GetLastError()); return 0; }
        log_line("WebView2Loader.dll 从系统路径加载");
    } else {
        log_line("WebView2Loader.dll 从 exe 同目录加载");
    }
    g_hLoader = h;
    g_pCreateEnv  = (PFN_CreateEnv)GetProcAddress(h, "CreateCoreWebView2EnvironmentWithOptions");
    g_pGetVersion = (PFN_GetVersion)GetProcAddress(h, "GetAvailableCoreWebView2BrowserVersionString");
    if (!g_pCreateEnv) { log_line("!! 找不到 CreateCoreWebView2EnvironmentWithOptions"); return 0; }
    return 1;
}

void init_webview(void)
{
    WCHAR userData[MAX_PATH + 32];
    EnvCB *cb;

    if (g_pGetVersion) {
        LPWSTR ver = NULL;
        if (SUCCEEDED(g_pGetVersion(NULL, &ver)) && ver) {
            log_line("WebView2 Runtime: %ls", ver);
            CoTaskMemFree(ver);
        } else {
            log_line("!! 未检测到 WebView2 Runtime");
        }
    }

    swprintf(userData, MAX_PATH + 32, L"%s\\webview_data", g_exeDir);
    cb = (EnvCB *)calloc(1, sizeof(EnvCB));
    if (!cb) return;
    cb->iface.lpVtbl = &g_EnvCBVtbl;
    cb->ref = 1;
    if (FAILED(g_pCreateEnv(NULL, userData, NULL,
            (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)cb)))
        log_line("!! CreateCoreWebView2EnvironmentWithOptions 立即失败");
}

void load_app_state(void)
{
    char *cfg_path = config_get_default_path();
    g.config = config_load(cfg_path);
    free(cfg_path);
    if (!g.config) { log_line("!! 配置加载失败"); return; }
    log_line("配置: projects_dir=%s auto_save=%d",
             g.config->projects_dir ? g.config->projects_dir : "(null)",
             g.config->auto_save_interval);

    /* 把持久化的导出目录同步到运行时副本（g.export_dir 供 do_export / 前端下发用） */
    if (g.config->export_dir && *g.config->export_dir)
        snprintf(g.export_dir, sizeof(g.export_dir), "%s", g.config->export_dir);

    g.engine = engine_create(g.config);
    if (g.engine) engine_set_progress_callback(g.engine, on_progress, NULL);
    if (g.engine) engine_set_chapter_delta_callback(g.engine, on_chapter_delta, NULL);
    if (g.engine) engine_set_text_delta_callback(g.engine, on_text_delta, NULL);
    else log_line("!! 引擎创建失败");

    g.selected_chapter = -1;
    g.last_doc[0] = g.last_txt[0] = g.last_epub[0] = '\0';
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    WCHAR *p;
    (void)hPrev;

    if (lpCmdLine && strstr(lpCmdLine, "--shot")) g_shotMode = 1;
    if (lpCmdLine && strstr(lpCmdLine, "--vhost")) g_useVHost = 1;
    if (lpCmdLine && strstr(lpCmdLine, "--hold"))  g_holdMode = 1;
    if (lpCmdLine && strstr(lpCmdLine, "--file"))  g_useFileUrl = 1;

    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    p = wcsrchr(g_exeDir, L'\\');
    if (p) *p = L'\0';

    g_uiThreadId = GetCurrentThreadId();
    InitializeCriticalSection(&g_askCs);
    g_askCsReady = 1;

    log_line("=== novelnow WebUI 启动 ===");
    log_line("exe 目录: %ls", g_exeDir);
    log_line("参数: shot=%d vhost=%d hold=%d file=%d",
             g_shotMode, g_useVHost, g_holdMode, g_useFileUrl);

    enable_dpi_awareness();
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
        log_line("!! CoInitializeEx 失败");

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"novelnowWebUI";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(0, L"novelnowWebUI",
                           L"novelnow · AI 小说写作助手",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1360, 880,
                           NULL, NULL, hInst, NULL);
    if (!hwnd) { log_line("!! CreateWindow 失败"); return 1; }
    g.hwnd = hwnd;
    g_hwnd = hwnd;

    /* --shot 由自动化调用，进程可能拿到 SW_HIDE 的 nCmdShow；
       更关键的是：窗口若被其它全屏窗口遮挡，WebView2 合成器不产帧，
       CapturePreview/CDP 会一直挂着不回。故置顶 + 前台。 */
    if (g_shotMode) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetWindowPos(hwnd, HWND_TOPMOST, 40, 40, 1360, 880,
                     SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
    } else {
        ShowWindow(hwnd, nCmdShow);
    }
    UpdateWindow(hwnd);
    enable_dark_titlebar(hwnd);

    /* 业务层先就位，再起 WebView2 */
    load_app_state();
    restart_autosave();

    if (!load_loader()) {
        MessageBoxW(hwnd,
            L"WebView2Loader.dll 加载失败。\n\n"
            L"请确认该 DLL 与 exe 在同一目录。",
            L"novelnow", MB_OK | MB_ICONERROR);
        return 1;
    }
    init_webview();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hLoader) FreeLibrary(g_hLoader);
    if (g_askCsReady) { DeleteCriticalSection(&g_askCs); g_askCsReady = 0; }
    CoUninitialize();
    log_line("=== novelnow WebUI 退出 ===");
    return 0;
}
