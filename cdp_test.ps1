# 用 Edge CDP 在真实浏览器环境中测试深度思考渲染
$edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
$proc = Start-Process $edge -ArgumentList "--headless=new","--disable-gpu","--remote-debugging-port=9223","--no-first-run","--user-data-dir=$env:TEMP\edgecdp","http://127.0.0.1:9090/" -PassThru
Start-Sleep -Seconds 5

$targets = Invoke-RestMethod "http://127.0.0.1:9223/json" -TimeoutSec 10
$page = $targets | Where-Object { $_.type -eq "page" -and $_.url -like "*9090*" } | Select-Object -First 1
if (-not $page) { Write-Host "NO PAGE TARGET"; $proc.Kill(); exit 1 }
Write-Host "page: $($page.url)"

$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$ct = [System.Threading.CancellationToken]::None
$ws.ConnectAsync([uri]$page.webSocketDebuggerUrl, $ct).Wait()

function Eval-JS($expr) {
  $cmd = @{ id = 1; method = "Runtime.evaluate"; params = @{ expression = $expr; returnByValue = $true } } | ConvertTo-Json -Depth 6 -Compress
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($cmd)
  $ws.SendAsync([ArraySegment[byte]]::new($bytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).Wait()
  $buf = New-Object byte[] 262144
  $res = $ws.ReceiveAsync([ArraySegment[byte]]::new($buf), $ct).Result
  [System.Text.Encoding]::UTF8.GetString($buf, 0, $res.Count)
}

# 1) 完整回答（思考已闭合）
$r1 = Eval-JS "renderAssistant('<thinking>正在分析用户的问题...</thinking>' + String.fromCharCode(10,10) + '**最终答案**：斐波那契函数如下')"
$j1 = $r1 | ConvertFrom-Json
$html1 = $j1.result.result.value
Write-Host "=== 闭合思考块测试:"
Write-Host "含 thinking details: $($html1 -match 'details class=.thinking')"
Write-Host "含思考正文: $($html1 -match '正在分析用户的问题')"
Write-Host "含最终答案: $($html1 -match '最终答案')"
Write-Host "含展开提示: $($html1 -match '已深度思考')"
Write-Host "无转圈(已结束): $(-not ($html1 -match '正在思考…'))"

# 2) 生成中（思考未闭合）
$r2 = Eval-JS "renderAssistant('<thinking>还在想...')"
$j2 = $r2 | ConvertFrom-Json
$html2 = $j2.result.result.value
Write-Host "=== 未闭合思考块测试:"
Write-Host "details 处于 open 状态: $($html2 -match 'details class=.thinking open')"
Write-Host "显示正在思考: $($html2 -match '正在思考…')"

# 3) 输入框滚动条隐藏验证（计算样式）
$r3 = Eval-JS "getComputedStyle(document.getElementById('input')).overflowY + '|' + getComputedStyle(document.getElementById('input')).maxHeight"
$j3 = $r3 | ConvertFrom-Json
Write-Host "=== 输入框样式: $($j3.result.result.value)"

$ws.Dispose()
$proc.Kill()
