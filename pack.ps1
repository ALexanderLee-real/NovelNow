# pack.ps1 —— NovelNow 打包（单包；galnow 演出层 2026-11-12 退役，两版分叉随之作废）
# 用法：pwsh -File pack.ps1 [-Version 0.2.13] [-OutDir "C:\Users\ROG\dsh-artifacts"]
# 约定（HANDOFF 传承）：
#   - 旧 zip 一律不删（旧版本留档）
#   - config.json（含 API key）/webui.log/projects/webview_data 绝不进包
#   - 产物默认落 C:\Users\ROG\dsh-artifacts\（2026-10-31 大哥哥定的产物规矩）

param(
    [string]$Version = "",
    [string]$OutDir = "C:\Users\ROG\dsh-artifacts"
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

$stage = Join-Path $root "_packtemp\novelnow-win64-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item (Join-Path $bin "novelnow.exe") $stage
Copy-Item (Join-Path $bin "WebView2Loader.dll") $stage
Copy-Item (Join-Path $bin "ui") (Join-Path $stage "ui") -Recurse

# 红线自检：绝不把 config.json（含 API key）/运行残留带进包
foreach ($bad in @("config.json", "webui.log", "projects", "webview_data", "舞台图片")) {
    $p = Join-Path $stage $bad
    if (Test-Path $p) { throw "红线：$bad 混进了打包暂存区" }
}
# 金丝雀：galnow 演出层已退役，不许借 bin\ui 陈旧文件混进包
foreach ($dead in @("ui\gal.js", "ui\novel.js", "ui\stage.js", "ui\comfy.js")) {
    if (Test-Path (Join-Path $stage $dead)) { throw "演出层已退役：$dead 混进了暂存区（bin\ui 陈旧文件泄漏），先跑 build 清场" }
}
$chk = [IO.File]::ReadAllText((Join-Path $stage "ui\index.html"))
if ($chk -match '(?i)galnow') { throw "index.html 仍有 galnow 残留" }
if ($chk -match '<script src="(novel|stage|comfy|gal)\.js"') { throw "index.html 仍引用演出层脚本" }

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
$zip = Join-Path $OutDir "novelnow-win64-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host ("novelnow-win64：{0}  ({1} 字节)" -f $zip, (Get-Item $zip).Length)
Write-Host "打包完成（旧 zip 未删）。"
