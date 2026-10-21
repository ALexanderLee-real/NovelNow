# pack_variants.ps1 —— NovelNow 两版并行打包（novel 纯小说版 / SinkNow 全量版）
# 用法：pwsh -File pack_variants.ps1 [-Version 0.2.13] [-OutDir "C:\Users\ROG\Desktop"]
# 约定（HANDOFF 传承）：
#   - 旧 zip 一律不删（旧版本留档）
#   - config.json（含 API key）/webui.log/projects/webview_data 绝不进包
#   - novel 版剔除 gal.js/comfy.js + index.html 的 galnow nav/section/script/注释
#   - 产物：novelnow-win64-<ver>-novel.zip 与 novelnow-win64-<ver>-SinkNow.zip

param(
    [string]$Version = "",
    [string]$OutDir = "C:\Users\ROG\Desktop"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

if (-not $Version) {
    $src = Get-Content (Join-Path $root "src\host_internal.h") -Raw
    if ($src -match 'NW_VERSION L"([\d.]+)"') { $Version = $Matches[1] }
    if (-not $Version) { throw "无法从 src\host_internal.h 解析 NW_VERSION，请显式传 -Version" }
}
Write-Host "打包版本：$Version"

$bin = Join-Path $root "bin"
if (-not (Test-Path (Join-Path $bin "novelnow.exe"))) { throw "先跑 build_msvc.bat 出 bin\novelnow.exe" }

$packtemp = Join-Path $root "_packtemp"

function Copy-BaseStage($stage) {
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item (Join-Path $bin "novelnow.exe") $stage
    Copy-Item (Join-Path $bin "WebView2Loader.dll") $stage
    Copy-Item (Join-Path $bin "ui") (Join-Path $stage "ui") -Recurse
    # 红线自检：绝不把 config.json 带进包
    foreach ($bad in @("config.json", "webui.log", "projects", "webview_data", "舞台图片")) {
        $p = Join-Path $stage $bad
        if (Test-Path $p) { throw "红线：$bad 混进了打包暂存区" }
    }
}

function Strip-Galnow($indexPath) {
    # 剥 nav 按钮块（data-page="galnow"，内无嵌套 button）
    $html = [IO.File]::ReadAllText($indexPath)
    $html = [regex]::Replace($html, '(?s)[ \t]*<button class="nav-item" data-page="galnow">.*?</button>\r?\n?', '')
    # 剥 GalNow 注释 + 整个 galnow section（区内无嵌套 </section>，取首个即止）
    $html = [regex]::Replace($html, '(?s)[ \t]*<!-- -+ GalNow.*?</section>\r?\n?', '')
    # 剥 gal.js / comfe.js 引用（comfy.js 全名另剥）
    $html = [regex]::Replace($html, '(?m)^[ \t]*<script src="gal\.js"></script>\r?\n?', '')
    $html = [regex]::Replace($html, '(?m)^[ \t]*<script src="comfy\.js"></script>\r?\n?', '')
    [IO.File]::WriteAllText($indexPath, $html, (New-Object System.Text.UTF8Encoding $false))
}

# ---- SinkNow 全量版 ----
$stageFull = Join-Path $packtemp "novelnow-win64-$Version-SinkNow"
Copy-BaseStage $stageFull
$zipFull = Join-Path $OutDir "novelnow-win64-$Version-SinkNow.zip"
if (Test-Path $zipFull) { Remove-Item $zipFull -Force }
Compress-Archive -Path (Join-Path $stageFull "*") -DestinationPath $zipFull
Write-Host ("SinkNow 全量版：{0}  ({1} 字节)" -f $zipFull, (Get-Item $zipFull).Length)

# ---- novel 纯小说版 ----
$stageNovel = Join-Path $packtemp "novelnow-win64-$Version-novel"
Copy-BaseStage $stageNovel
Remove-Item (Join-Path $stageNovel "ui\gal.js") -Force
Remove-Item (Join-Path $stageNovel "ui\comfy.js") -Force
Strip-Galnow (Join-Path $stageNovel "ui\index.html")
# 金丝雀自检：novel 版 index.html 不许再有 galnow 残留
$chk = [IO.File]::ReadAllText((Join-Path $stageNovel "ui\index.html"))
if ($chk -match '(?i)galnow') { throw "novel 版 index.html 仍有 galnow 残留，剥除失败" }
$zipNovel = Join-Path $OutDir "novelnow-win64-$Version-novel.zip"
if (Test-Path $zipNovel) { Remove-Item $zipNovel -Force }
Compress-Archive -Path (Join-Path $stageNovel "*") -DestinationPath $zipNovel
Write-Host ("novel 纯小说版：{0}  ({1} 字节)" -f $zipNovel, (Get-Item $zipNovel).Length)

Write-Host "打包完成（旧 zip 未删）。"
