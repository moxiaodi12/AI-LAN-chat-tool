# 最终 E2E 测试脚本
$ErrorActionPreference = "Stop"
$base = "http://127.0.0.1:9090"

Write-Host "=== 1. 首页 ==="
$h = Invoke-WebRequest -Uri "$base/" -TimeoutSec 10
Write-Host "HTTP $($h.StatusCode) 长度 $($h.Content.Length)"

Write-Host "=== 2. 第一轮对话 ==="
$sys = "你是一个运行在用户本地电脑上的 AI 助手（DeepSeek-R1）。请优先使用中文回答。"
$m1 = @(
  @{ role = "system"; content = $sys },
  @{ role = "user"; content = "我喜欢的数字是 42" }
)
$obj1 = @{ client = "final-user-1"; messages = $m1; max_tokens = 512 } | ConvertTo-Json -Depth 5
$r1 = Invoke-RestMethod -Uri "$base/api/chat" -Method Post -ContentType "application/json; charset=utf-8" -Body ([System.Text.Encoding]::UTF8.GetBytes($obj1)) -TimeoutSec 10
Write-Host "ticket=$($r1.ticket)"
$res1 = $null
for ($i = 0; $i -lt 60; $i++) {
  Start-Sleep -Seconds 2
  $res1 = Invoke-RestMethod -Uri "$base/api/result?ticket=$($r1.ticket)" -TimeoutSec 5
  if ($res1.state -ne "queued" -and $res1.state -ne "generating") { break }
}
Write-Host "第一轮: state=$($res1.state) prompt=$($res1.prompt_tokens) comp=$($res1.completion_tokens)"
$a1 = $res1.text
if ($a1.Length -gt 60) { $a1 = $a1.Substring(0, 60) }
Write-Host "  输出: $($a1 -replace "`r?`n", " ")"

Write-Host "=== 3. 第二轮对话（引用第一轮内容，验证连续对话）==="
$prev = $res1.text
$m2 = @(
  @{ role = "system"; content = $sys },
  @{ role = "user"; content = "我喜欢的数字是 42" },
  @{ role = "assistant"; content = $prev },
  @{ role = "user"; content = "我刚才说的数字是多少？只回答数字" }
)
$obj2 = @{ client = "final-user-1"; messages = $m2; max_tokens = 200 } | ConvertTo-Json -Depth 5
$r2 = Invoke-RestMethod -Uri "$base/api/chat" -Method Post -ContentType "application/json; charset=utf-8" -Body ([System.Text.Encoding]::UTF8.GetBytes($obj2)) -TimeoutSec 10
$res2 = $null
for ($i = 0; $i -lt 60; $i++) {
  Start-Sleep -Seconds 2
  $res2 = Invoke-RestMethod -Uri "$base/api/result?ticket=$($r2.ticket)" -TimeoutSec 5
  if ($res2.state -ne "queued" -and $res2.state -ne "generating") { break }
}
$a2 = $res2.text
if ($a2.Length -gt 100) { $a2 = $a2.Substring(0, 100) }
Write-Host "第二轮: state=$($res2.state) 回答: $($a2 -replace "`r?`n", " ")"

Write-Host "=== 4. 状态接口 ==="
$st = Invoke-RestMethod -Uri "$base/api/status?client=final-user-1" -TimeoutSec 5
Write-Host "llama_ok=$($st.llama_ok) ctx=$($st.ctx_size) queue=$($st.queue_len) pos=$($st.your_position)"

Write-Host "=== 全部测试完成 ==="
