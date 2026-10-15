# NovelNow

一款以 C 語言編寫、基於 WebView2 圖形介面的 Windows AI 輔助小說寫作工具。

**介面語言**：English（[英文版說明](README.md)）· 簡體中文 · 繁體中文 · 日本語 —— 預設為英文，可隨時在側邊欄切換，選擇會保存在本機。

> **這只是一個前瞻版本，很多功能還沒有做。**我正朝著一個能實現你想法、並且隨時開始流式閱讀的專案發展。這可能需要一個有長上下文文本處理能力、同時有高 token 輸出速度的 AI 來輔助它運行，否則你只能體驗到它基礎的寫作能力。**本專案採用 GPLv3 授權條款，請注意。**

## 功能特性

- AI 驅動的章節寫作與擴寫，支援流式輸出
- 前文記憶壓縮：逐章梗概與伏筆登記自動注入提示詞
- 伏筆登記表：追蹤已埋設與已回收的劇情線索
- 秒停取消：隨時中止進行中的 AI 請求
- 相容任意 OpenAI 格式的 API——內建 OpenRouter 免費模型預設

## 快速開始

### 執行已編譯版本

1. 下載最新的 release 壓縮包並解壓
2. 將 `config.default.json` 重新命名為 `config.json`，並填入你的 API Key
3. 執行 `novelnow.exe`（需 Windows 10 以上，並安裝 WebView2 執行時，多數系統已內建）

### 從原始碼建置

環境需求：

- Visual Studio 2022（含 C/C++ 工具集 MSVC）
- Windows SDK

步驟：

```bat
git clone https://github.com/ALexanderLee-real/NovelNow.git
cd NovelNow
build_msvc.bat
```

建置指令碼會透過 `vswhere` 定位 VS2022、載入 `vcvars64`，並連結內建的 WebView2 載入器（`third_party/webview2/x64`）。

## 專案結構

```
├── src/          C 原始碼（引擎、AI API 客戶端、專案儲存、宿主）
├── include/      內部標頭檔（cJSON、WebView2 繫結）
├── ui/           前端資源（HTML/CSS/JS），於 WebView2 內呈現
├── third_party/  WebView2 SDK（標頭檔與載入器）
├── build_msvc.bat  一鍵 MSVC 建置指令碼
└── config.default.json  設定範本
```

## 設定說明

將 `config.default.json` 複製為 `config.json`（放在執行檔旁邊）並調整：

| 欄位 | 說明 |
|---|---|
| `ai.api_url` | OpenAI 相容的 API 位址，例如 `https://api.deepseek.com` |
| `ai.api_key` | 你的 API Key（僅保存在本機，不會上傳） |
| `ai.model` | 模型 ID，或從 OpenRouter 免費預設中選擇 |

## 授權條款

本專案採用 [GNU General Public License v3.0](LICENSE) 授權。
