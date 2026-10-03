<#
    pack.ps1 - 组装可运行的 AI-Chat-Server 产物文件夹

    把源码文件夹里的运行时资源（web、llama、models、config.ini、
    start.bat、使用说明.txt）同步到产物文件夹。使用增量复制，
    只覆盖有变化的文件，绝不删除产物文件夹里已有的东西
    （尤其是 models 里的 gguf 模型）。

    一般由 build.bat 自动调用，也可以单独运行：
        powershell -ExecutionPolicy Bypass -File .\pack.ps1
        powershell -ExecutionPolicy Bypass -File .\pack.ps1 -Product D:\out\AI-Chat-Server
#>
[CmdletBinding()]
param(
    [string]$Source,
    [string]$Product
)

$ErrorActionPreference = 'Stop'

# 默认：源码文件夹 = 本脚本所在目录；产物文件夹 = 源码文件夹的同级 AI-Chat-Server
if (-not $Source) { $Source = $PSScriptRoot }
$Source = (Resolve-Path -LiteralPath $Source).Path

if (-not $Product) {
    $Product = Join-Path (Split-Path -Parent $Source) 'AI-Chat-Server'
}

if (-not (Test-Path -LiteralPath $Product)) {
    New-Item -ItemType Directory -Force -Path $Product | Out-Null
}
$Product = (Resolve-Path -LiteralPath $Product).Path

if ($Source -eq $Product) {
    throw "源码文件夹和产物文件夹不能是同一个目录：$Source"
}

# 增量同步整个目录树；不用 /MIR，所以不会删除目标里多出来的文件
function Sync-Tree {
    param([string]$From, [string]$To)

    if (-not (Test-Path -LiteralPath $To)) {
        New-Item -ItemType Directory -Force -Path $To | Out-Null
    }
    # /E 含空子目录  /XO 跳过未变化的文件  /R:1 /W:1 失败不长时间重试
    $null = robocopy $From $To /E /XO /R:1 /W:1 /NFL /NDL /NJH /NJS /NP
    if ($LASTEXITCODE -ge 8) {
        throw "复制失败（robocopy 退出码 $LASTEXITCODE）：$From -> $To"
    }
    return (Get-ChildItem -LiteralPath $To -Recurse -File | Measure-Object).Count
}

Write-Host "  源码 : $Source"
Write-Host "  产物 : $Product"

$report = @()

# ---------- 运行时目录 ----------
foreach ($dir in @('web', 'llama', 'models')) {
    $from = Join-Path $Source $dir
    if (-not (Test-Path -LiteralPath $from)) {
        Write-Host "  [跳过] 源码里没有 $dir\"
        continue
    }
    $n = Sync-Tree -From $from -To (Join-Path $Product $dir)
    $report += "  $dir\  ($n 个文件)"
}

# ---------- 单个文件 ----------
foreach ($file in @('config.ini', 'start.bat', '使用说明.txt')) {
    $from = Join-Path $Source $file
    if (-not (Test-Path -LiteralPath $from)) { continue }
    Copy-Item -LiteralPath $from -Destination (Join-Path $Product $file) -Force
    $report += "  $file"
}

Write-Host "  已同步："
$report | ForEach-Object { Write-Host $_ }

# ---------- 检查模型 ----------
$modelDir = Join-Path $Product 'models'
$gguf = @()
if (Test-Path -LiteralPath $modelDir) {
    $gguf = @(Get-ChildItem -LiteralPath $modelDir -Filter '*.gguf' -File -ErrorAction SilentlyContinue)
}

if ($gguf.Count -eq 0) {
    $cfg = Join-Path $Product 'config.ini'
    $want = '(config.ini 里 Model 指定的文件)'
    if (Test-Path -LiteralPath $cfg) {
        $line = Select-String -LiteralPath $cfg -Pattern '^\s*Model\s*=' | Select-Object -First 1
        if ($line) { $want = ($line.Line -split '=', 2)[1].Trim() }
    }
    Write-Host ""
    Write-Host "  [注意] 产物 models\ 里还没有模型文件，程序能启动但无法对话。" -ForegroundColor Yellow
    Write-Host "         请把 $want" -ForegroundColor Yellow
    Write-Host "         放到 $modelDir\" -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host "  模型：$($gguf[0].Name)  ($([math]::Round($gguf[0].Length / 1MB)) MB)"
}
