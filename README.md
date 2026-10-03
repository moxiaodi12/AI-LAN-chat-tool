# 离线版局域网 AI 聊天工具

一个完全离线运行的局域网 AI 聊天工具，基于 llama.cpp 推理引擎打造。只需在一台 Windows 电脑上运行本服务，局域网内其他设备用浏览器即可访问聊天，无需安装任何客户端软件。

## 📖 项目简介

本项目专为受限局域网（如学校机房、企业内部网络）设计，提供完全离线的 AI 对话服务。
- **完全离线**：无需连接互联网，保护数据隐私。
- **开箱即用**：双击启动脚本即可运行，其他设备仅需浏览器。
- **纯 CPU 算力**：内置 DeepSeek-R1-Distill-Qwen-1.5B (Q3_K_M) 模型，无需独立显卡。
- **队列机制**：同一时间只处理一个用户的对话，其他用户自动排队，保证服务稳定。
- **本地存储**：聊天记录保存在各自的浏览器缓存中，不清除浏览器缓存即可一直保留。

## ✨ 核心功能

- **连续对话**：AI 能记住上下文，直接接着问即可。
- **上下文监控**：左侧面板实时显示本轮对话的上下文占用。
- **排队状态**：实时显示你的排队位置和当前生成速度。
- **过程控制**：生成过程中可点击"■ 停止"中断；支持清空当前聊天记录、导出为文本。
- **思考过程**：AI 的"深度思考"内容可点击展开/收起。
- **代码辅助**：擅长 Python 代码生成，代码块支持一键复制。
- **MTP 支持**：内置 llama.cpp b10644 版本，支持 MTP（多 Token 预测）投机解码，搭配带 MTP 头的 GGUF 模型可显著提升生成速度。

## 🚀 快速开始

### 运行环境要求
- 操作系统：Windows 7 及以上（64 位）
- 内存：建议至少 3GB 空闲内存
- 浏览器：Chrome / Edge

### 如何启动
**方法一：双击 `start.bat`**
**方法二：双击 `server.exe`**

启动后，控制台会显示访问网址：
```text
本机访问:    http://127.0.0.1:9090
局域网访问:  http://192.168.x.x:9090
```
把“局域网访问”的地址发给同事或朋友，用浏览器打开即可聊天。

## 📁 文件夹说明
```text  
AI-Chat-Server/  
├─ server.exe        主程序（MinGW-w64 静态编译，无需任何运行库）  
├─ start.bat         一键启动脚本  
├─ config.ini        配置文件（端口、上下文大小等）  
├─ web/  
│   └─ index.html    网页界面（独立文件，可自行美化）  
├─ llama/            内置 llama.cpp 推理引擎及运行库  
├─ models/           模型文件存放处（GGUF 格式）  
├─ server.log        运行日志（自动追加）  
├─ build.bat         一键编译+组装脚本（源码编译用）  
├─ server.cpp        主程序源码（C++17，单文件）  
├─ json.hpp          第三方支持库（nlohmann/json）  
├─ cdp_test.ps1      浏览器调试脚本（开发用）  
└─ e2e_test.ps1      接口测试脚本（需服务器已启动）
```
## ⚙️ 配置文件说明 (config.ini)
配置项	默认值	说明  
Port	9090	HTTP 端口。被占用时自动每次 +5（9095、9100...）直到找到空闲端口  
CtxSize	4096	上下文窗口（tokens）。改大会占更多内存  
MaxTokens	2048	单次回答最大长度（tokens）  
Model	...	模型文件名（放在 models 文件夹内）  
Mtp	0	MTP（多 token 预测）投机解码开关，1 为开启  
MtpDraft	3	每次草稿预测 token 数（Mtp=1 时生效）  
修改后需重启服务器生效。  

## 🛠️ 源码编译指南（可选）
如果你希望从源码自行编译，请参考以下步骤：

### 一、安装编译环境
本程序使用 MinGW-w64 (g++) 静态编译，不能用 MSVC (cl.exe)。

下载 WinLibs 版 MinGW-w64（推荐 UCRT runtime + x86_64 版本）。

解压到不含空格和中文的路径，例如 C:\mingw64。

将 C:\mingw64\bin 加入系统环境变量 PATH。

验证：新开命令行窗口，输入 g++ --version 看是否正常。

### 二、编译与组装
双击 源码\build.bat 即可。它会自动查找 g++、编译 server.cpp 并调用 pack.ps1 组装产物。

如果你想手动编译，可使用以下命令（与 build.bat 等效）：

```bash
g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -s ^
    -o "..\AI-Chat-Server\server.exe" server.cpp ^
    -lws2_32 -liphlpapi -lwinpthread
```
### 三、模型准备
模型 GGUF 文件（约 882MB）未包含在源码包中，需要单独准备：

方式一：直接放到产物文件夹 ..\AI-Chat-Server\models\ 下。

方式二：放到源码包的 models\ 里，每次编译自动带入产物。

换模型：把 gguf 放进 models\，改 config.ini 的 Model 一行，重启。

## ❓ 常见问题
### 1. 其他电脑打不开网页？

检查两台电脑是否在同一局域网。

检查本机 Windows 防火墙是否放行了 server.exe（首次运行时弹出提示请点“允许访问”）。

确认访问的是控制台显示的“局域网访问”地址。

### 2. 启动失败？

看控制台日志：模型文件缺失、llama 文件夹不完整都会给出提示。

内存不足：请保证至少 3GB 空闲内存。

### 3. 回复很慢？

纯 CPU 推理，速度取决于电脑 CPU 性能。首次提问会稍慢（模型加载），之后正常。

可减小“输出”长度或换更小的模型。

### 4. 换电脑使用？
直接把整个 AI-Chat-Server 文件夹拷到任意 Windows 电脑，双击 start.bat 即可，不需要安装任何运行库。

## 📄 其他
博客文章：[CSDN博客链接](https://blog.csdn.net/2401_84336995/article/details/164121068?fromshare=blogdetail&sharetype=blogdetail&sharerId=164121068&sharerefer=PC&sharesource=2401_84336995&sharefrom=from_link)
