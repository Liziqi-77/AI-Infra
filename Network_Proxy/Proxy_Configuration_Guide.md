# 网络代理（Proxy）配置详解

## 目录
1. [什么是代理](#1-什么是代理)
2. [为什么要配置代理](#2-为什么要配置代理)
3. [代理的类型](#3-代理的类型)
4. [配置代理的方式](#4-配置代理的方式)
5. [配置代理的场景](#5-配置代理的场景)
6. [配置代理的作用](#6-配置代理的作用)
7. [代理配置的环境变量详解](#7-代理配置的环境变量详解)
8. [常见代理配置示例](#8-常见代理配置示例)
9. [代理配置的注意事项](#9-代理配置的注意事项)
10. [代理故障排查](#10-代理故障排查)
11. [代理安全性考虑](#11-代理安全性考虑)
12. [总结](#12-总结)

---

## 1. 什么是代理

### 1.1 基本概念

**代理（Proxy）** 是一种网络服务，它充当客户端和服务器之间的中间人。当客户端需要访问某个服务器时，请求首先发送到代理服务器，然后由代理服务器转发请求到目标服务器，并将响应返回给客户端。

### 1.2 代理的工作原理

```
客户端 (Client)
    │
    │ HTTP/HTTPS 请求
    ▼
代理服务器 (Proxy Server)
    │
    │ 转发请求
    ▼
目标服务器 (Target Server)
    │
    │ 响应
    ▼
代理服务器 (Proxy Server)
    │
    │ 返回响应
    ▼
客户端 (Client)
```

### 1.3 代理的核心特征

- **中间层**：代理位于客户端和目标服务器之间
- **转发功能**：代理接收请求并转发到目标服务器
- **可配置性**：可以设置规则来决定哪些请求需要通过代理
- **透明性**：对客户端来说，代理可以完全透明，也可以提供额外功能

---

## 2. 为什么要配置代理

### 2.1 网络访问限制

**企业内网环境**
- 公司网络通常有防火墙限制，直接访问外网可能被阻止
- 需要通过公司提供的代理服务器才能访问外部资源
- 确保网络流量经过安全审计和监控

**地理位置限制**
- 某些服务可能对特定地区有访问限制
- 通过代理可以绕过地理限制
- 访问被封锁的网站或服务

### 2.2 安全性和隐私保护

**匿名性**
- 隐藏客户端的真实IP地址
- 保护用户隐私和身份信息
- 防止目标服务器追踪用户位置

**安全过滤**
- 代理服务器可以过滤恶意内容
- 阻止访问不安全的网站
- 提供额外的安全层

### 2.3 性能优化

**缓存功能**
- 代理服务器可以缓存常用资源
- 减少重复请求，提高访问速度
- 降低带宽消耗

**负载均衡**
- 多个客户端可以通过同一个代理访问
- 代理可以分发请求到多个服务器
- 提高整体系统性能

### 2.4 访问控制和审计

**访问日志**
- 记录所有通过代理的网络请求
- 便于审计和监控
- 符合企业合规要求

**访问控制**
- 限制某些用户或应用的网络访问
- 实施网络使用策略
- 防止未授权访问

---

## 3. 代理的类型

### 3.1 HTTP 代理

**特点**
- 主要用于HTTP协议
- 可以处理HTTP请求和响应
- 支持缓存和内容过滤

**使用场景**
- Web浏览
- API调用（HTTP）
- 文件下载

### 3.2 HTTPS 代理

**特点**
- 支持HTTPS加密连接
- 可以处理SSL/TLS加密流量
- 提供端到端加密保护

**使用场景**
- 安全Web访问
- 加密API调用
- 安全文件传输

### 3.3 SOCKS 代理

**特点**
- 更底层的代理协议
- 支持多种协议（TCP/UDP）
- 不解析应用层数据

**使用场景**
- 游戏连接
- P2P应用
- 需要底层网络访问的应用

### 3.4 正向代理 vs 反向代理

**正向代理（Forward Proxy）**
- 客户端知道代理的存在
- 客户端主动配置代理
- 用于访问外部资源

**反向代理（Reverse Proxy）**
- 客户端不知道代理的存在
- 服务器端配置
- 用于负载均衡和缓存

---

## 4. 配置代理的方式

### 4.1 环境变量配置（最常用）

这是您提供的配置方式，适用于Linux/Unix/macOS系统和Windows的Git Bash、WSL等环境。

#### 4.1.1 基本配置格式

```bash
# Set base proxy URL with authentication
export BASE_PROXY="http://<username>:<password>@<proxy_host>:<port>"

# HTTP proxy settings (lowercase for Unix-like systems)
export http_proxy=$BASE_PROXY
export https_proxy=$BASE_PROXY

# HTTP proxy settings (uppercase for some applications)
export HTTP_PROXY=$BASE_PROXY
export HTTPS_PROXY=$BASE_PROXY

# No proxy settings - addresses that bypass the proxy
export no_proxy="localhost,127.0.0.1,.corp.com,10.0.0.0/8"
export NO_PROXY="localhost,127.0.0.1,.corp.com,10.0.0.0/8"
```

#### 4.1.2 配置说明

**BASE_PROXY 变量**
- 存储代理服务器的完整URL
- 包含认证信息（用户名和密码）
- 格式：`http://username:password@host:port`

**http_proxy / HTTP_PROXY**
- 配置HTTP协议的代理
- 小写版本用于Unix/Linux系统
- 大写版本用于某些应用程序

**https_proxy / HTTPS_PROXY**
- 配置HTTPS协议的代理
- 确保加密连接也通过代理

**no_proxy / NO_PROXY**
- 指定不需要通过代理的地址
- 支持域名、IP地址、CIDR格式
- 提高本地访问效率

### 4.2 配置文件方式

#### 4.2.1 Shell配置文件（永久配置）

**Bash配置（~/.bashrc 或 ~/.bash_profile）**
```bash
# Add proxy configuration
export BASE_PROXY="http://username:password@proxy.example.com:8080"
export http_proxy=$BASE_PROXY
export https_proxy=$BASE_PROXY
export HTTP_PROXY=$BASE_PROXY
export HTTPS_PROXY=$BASE_PROXY
export no_proxy="localhost,127.0.0.1,.corp.com,10.0.0.0/8"
```

**Zsh配置（~/.zshrc）**
```bash
# Same configuration as bash
export BASE_PROXY="http://username:password@proxy.example.com:8080"
export http_proxy=$BASE_PROXY
export https_proxy=$BASE_PROXY
export HTTP_PROXY=$BASE_PROXY
export HTTPS_PROXY=$BASE_PROXY
export no_proxy="localhost,127.0.0.1,.corp.com,10.0.0.0/8"
```

#### 4.2.2 系统级配置（/etc/environment）

```bash
# System-wide proxy configuration
http_proxy="http://username:password@proxy.example.com:8080"
https_proxy="http://username:password@proxy.example.com:8080"
HTTP_PROXY="http://username:password@proxy.example.com:8080"
HTTPS_PROXY="http://username:password@proxy.example.com:8080"
no_proxy="localhost,127.0.0.1,.corp.com,10.0.0.0/8"
```

### 4.3 应用程序特定配置

#### 4.3.1 Git配置

```bash
# Configure Git to use proxy
git config --global http.proxy http://username:password@proxy.example.com:8080
git config --global https.proxy http://username:password@proxy.example.com:8080

# Configure Git to bypass proxy for specific domains
git config --global http.https://github.com.proxy ""
```

#### 4.3.2 npm配置

```bash
# Configure npm proxy
npm config set proxy http://username:password@proxy.example.com:8080
npm config set https-proxy http://username:password@proxy.example.com:8080

# Configure npm to bypass proxy for specific registry
npm config set registry https://registry.npmjs.org/
```

#### 4.3.3 pip配置

**命令行方式**
```bash
pip install --proxy http://username:password@proxy.example.com:8080 package_name
```

**配置文件方式（~/.pip/pip.conf 或 %APPDATA%\pip\pip.ini）**
```ini
[global]
proxy = http://username:password@proxy.example.com:8080
```

#### 4.3.4 Docker配置

**Docker daemon配置（/etc/docker/daemon.json）**
```json
{
  "proxies": {
    "http-proxy": "http://username:password@proxy.example.com:8080",
    "https-proxy": "http://username:password@proxy.example.com:8080",
    "no-proxy": "localhost,127.0.0.1,.corp.com"
  }
}
```

### 4.4 Windows系统配置

#### 4.4.1 系统设置
- 打开"设置" → "网络和Internet" → "代理"
- 配置手动代理设置
- 输入代理服务器地址和端口

#### 4.4.2 PowerShell配置
```powershell
# Set proxy environment variables
$env:HTTP_PROXY = "http://username:password@proxy.example.com:8080"
$env:HTTPS_PROXY = "http://username:password@proxy.example.com:8080"
$env:NO_PROXY = "localhost,127.0.0.1,.corp.com,10.0.0.0/8"

# Permanent configuration (User level)
[System.Environment]::SetEnvironmentVariable("HTTP_PROXY", "http://username:password@proxy.example.com:8080", "User")
[System.Environment]::SetEnvironmentVariable("HTTPS_PROXY", "http://username:password@proxy.example.com:8080", "User")
```

---

## 5. 配置代理的场景

### 5.1 企业内网环境

**场景描述**
- 公司网络有防火墙限制
- 需要通过公司代理访问外网
- 需要访问内部资源（.corp.com域名）

**配置要点**
```bash
export BASE_PROXY="http://employee:password@corp-proxy.company.com:8080"
export http_proxy=$BASE_PROXY
export https_proxy=$BASE_PROXY
export no_proxy="localhost,127.0.0.1,.corp.com,10.0.0.0/8,172.16.0.0/12"
```

**为什么需要no_proxy**
- `.corp.com`：公司内部域名，不需要代理
- `10.0.0.0/8`：内网IP段，直接访问更快
- `localhost,127.0.0.1`：本地服务，不需要代理

### 5.2 开发环境

**场景描述**
- 开发机器在企业网络中
- 需要下载依赖包（npm, pip, maven等）
- 需要访问Git仓库

**配置要点**
- 配置全局代理环境变量
- 配置各开发工具的代理设置
- 确保CI/CD系统也能使用代理

### 5.3 云服务器环境

**场景描述**
- 云服务器需要访问外部API
- 某些云服务商要求通过代理访问
- 需要访问被限制的资源

**配置要点**
- 在服务器启动脚本中配置代理
- 使用系统级配置文件
- 确保服务重启后代理配置仍然有效

### 5.4 容器化环境

**场景描述**
- Docker容器需要访问外部资源
- Kubernetes Pod需要网络访问
- 容器镜像拉取需要代理

**配置要点**
- 配置Docker daemon代理
- 在容器启动时传入代理环境变量
- 配置Kubernetes的代理设置

### 5.5 CI/CD环境

**场景描述**
- 持续集成服务器需要下载依赖
- 构建过程需要访问外部资源
- 部署过程需要网络访问

**配置要点**
- 在CI/CD配置文件中设置代理
- 使用环境变量或密钥管理
- 确保代理配置的安全性

---

## 6. 配置代理的作用

### 6.1 网络访问控制

**作用**
- 统一管理网络访问策略
- 控制哪些应用可以访问外网
- 实施访问时间限制

**实现方式**
- 通过代理服务器实施访问控制
- 记录所有网络请求日志
- 根据规则允许或拒绝请求

### 6.2 安全防护

**作用**
- 过滤恶意内容
- 阻止访问不安全网站
- 防止数据泄露

**实现方式**
- 代理服务器扫描内容
- 实施URL过滤
- 检测和阻止威胁

### 6.3 性能优化

**作用**
- 缓存常用资源
- 减少带宽消耗
- 提高访问速度

**实现方式**
- 代理服务器缓存静态资源
- 压缩传输内容
- 优化网络路径

### 6.4 审计和监控

**作用**
- 记录所有网络活动
- 分析网络使用情况
- 符合合规要求

**实现方式**
- 代理服务器记录日志
- 生成访问报告
- 监控异常行为

### 6.5 负载均衡

**作用**
- 分发网络请求
- 提高系统可用性
- 优化资源利用

**实现方式**
- 多个代理服务器
- 请求分发算法
- 健康检查机制

---

## 7. 代理配置的环境变量详解

### 7.1 环境变量命名规范

**大小写敏感性**
- Unix/Linux系统：通常使用小写（`http_proxy`, `https_proxy`）
- Windows系统：通常使用大写（`HTTP_PROXY`, `HTTPS_PROXY`）
- 最佳实践：同时设置大小写版本，确保兼容性

### 7.2 BASE_PROXY变量

**作用**
- 存储代理服务器的完整URL
- 便于统一管理和修改
- 避免重复配置

**格式**
```bash
export BASE_PROXY="http://username:password@proxy.example.com:8080"
```

**组成部分**
- `http://`：协议类型（也可以是`https://`或`socks5://`）
- `username:password@`：认证信息（可选）
- `proxy.example.com`：代理服务器地址
- `8080`：代理服务器端口

### 7.3 http_proxy / HTTP_PROXY

**作用**
- 配置HTTP协议的代理
- 影响所有HTTP请求
- 被大多数HTTP客户端识别

**使用场景**
- Web浏览器
- curl命令
- wget命令
- HTTP API调用

### 7.4 https_proxy / HTTPS_PROXY

**作用**
- 配置HTTPS协议的代理
- 处理加密连接
- 支持SSL/TLS隧道

**使用场景**
- 安全Web访问
- 加密API调用
- 安全文件传输

### 7.5 no_proxy / NO_PROXY

**作用**
- 指定不需要通过代理的地址
- 提高本地访问效率
- 避免代理循环

**格式说明**
```bash
export no_proxy="localhost,127.0.0.1,.corp.com,10.0.0.0/8"
```

**支持的格式**
- **域名**：`example.com`（精确匹配）
- **通配符域名**：`.corp.com`（匹配所有corp.com子域名）
- **IP地址**：`127.0.0.1`（精确匹配）
- **CIDR格式**：`10.0.0.0/8`（匹配整个IP段）
- **端口**：`example.com:8080`（指定端口）

**常见配置**
- `localhost,127.0.0.1`：本地服务
- `.corp.com,.local`：内部域名
- `10.0.0.0/8,172.16.0.0/12,192.168.0.0/16`：私有IP段

### 7.6 其他相关环境变量

**ALL_PROXY**
- 配置所有协议的代理
- 作为默认代理设置
- 当特定协议代理未设置时使用

**FTP_PROXY**
- 配置FTP协议的代理
- 用于FTP文件传输

**SOCKS_PROXY**
- 配置SOCKS代理
- 支持更底层的网络访问

---

## 8. 常见代理配置示例

### 8.1 企业内网代理配置

```bash
#!/bin/bash
# Enterprise network proxy configuration

# Set base proxy URL
export BASE_PROXY="http://username:password@proxy.company.com:8080"

# HTTP/HTTPS proxy
export http_proxy=$BASE_PROXY
export https_proxy=$BASE_PROXY
export HTTP_PROXY=$BASE_PROXY
export HTTPS_PROXY=$BASE_PROXY

# No proxy for internal resources
export no_proxy="localhost,127.0.0.1,.company.com,.corp.com,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"
export NO_PROXY=$no_proxy

# Additional proxy settings
export ALL_PROXY=$BASE_PROXY
```

### 8.2 开发环境代理配置

```bash
#!/bin/bash
# Development environment proxy configuration

# Proxy server settings
PROXY_HOST="dev-proxy.example.com"
PROXY_PORT="3128"
PROXY_USER="developer"
PROXY_PASS="dev_password"

# Build proxy URL
export BASE_PROXY="http://${PROXY_USER}:${PROXY_PASS}@${PROXY_HOST}:${PROXY_PORT}"

# Standard proxy variables
export http_proxy=$BASE_PROXY
export https_proxy=$BASE_PROXY
export HTTP_PROXY=$BASE_PROXY
export HTTPS_PROXY=$BASE_PROXY

# Bypass proxy for local development
export no_proxy="localhost,127.0.0.1,0.0.0.0,.local,*.local"
export NO_PROXY=$no_proxy

# Git proxy configuration
git config --global http.proxy $BASE_PROXY
git config --global https.proxy $BASE_PROXY

# npm proxy configuration
npm config set proxy $BASE_PROXY
npm config set https-proxy $BASE_PROXY
```

### 8.3 无认证代理配置

```bash
#!/bin/bash
# Proxy without authentication

export BASE_PROXY="http://proxy.example.com:8080"
export http_proxy=$BASE_PROXY
export https_proxy=$BASE_PROXY
export HTTP_PROXY=$BASE_PROXY
export HTTPS_PROXY=$BASE_PROXY
export no_proxy="localhost,127.0.0.1"
```

### 8.4 SOCKS代理配置

```bash
#!/bin/bash
# SOCKS proxy configuration

export BASE_PROXY="socks5://username:password@proxy.example.com:1080"
export http_proxy=$BASE_PROXY
export https_proxy=$BASE_PROXY
export HTTP_PROXY=$BASE_PROXY
export HTTPS_PROXY=$BASE_PROXY
export ALL_PROXY=$BASE_PROXY
```

### 8.5 临时代理配置（单次会话）

```bash
# Set proxy for current session only
export http_proxy="http://proxy.example.com:8080"
export https_proxy="http://proxy.example.com:8080"

# Test proxy connection
curl -I https://www.example.com

# Unset proxy when done
unset http_proxy
unset https_proxy
```

### 8.6 条件代理配置（脚本方式）

```bash
#!/bin/bash
# Conditional proxy configuration based on network

# Detect network location
if ping -c 1 proxy.company.com &> /dev/null; then
    # Corporate network - use proxy
    export BASE_PROXY="http://username:password@proxy.company.com:8080"
    export http_proxy=$BASE_PROXY
    export https_proxy=$BASE_PROXY
    export HTTP_PROXY=$BASE_PROXY
    export HTTPS_PROXY=$BASE_PROXY
    echo "Corporate network detected - proxy enabled"
else
    # Home network - no proxy
    unset http_proxy
    unset https_proxy
    unset HTTP_PROXY
    unset HTTPS_PROXY
    echo "Home network detected - proxy disabled"
fi
```

---

## 9. 代理配置的注意事项

### 9.1 安全性注意事项

**密码安全**
- ❌ **不要**在脚本中硬编码密码
- ✅ **使用**环境变量或密钥管理工具
- ✅ **使用**配置文件权限控制（chmod 600）

**推荐方式**
```bash
# Read credentials from secure file
source ~/.proxy_credentials

export BASE_PROXY="http://${PROXY_USER}:${PROXY_PASS}@${PROXY_HOST}:${PROXY_PORT}"
```

### 9.2 兼容性注意事项

**大小写问题**
- 同时设置大小写版本的环境变量
- 某些应用只识别大写，某些只识别小写
- 确保最大兼容性

**协议问题**
- HTTP代理通常也支持HTTPS
- 某些代理需要明确指定HTTPS代理
- 测试不同协议的连接

### 9.3 性能注意事项

**no_proxy配置**
- 正确配置no_proxy避免不必要的代理
- 本地服务不应该通过代理
- 内网资源直接访问更快

**代理服务器选择**
- 选择地理位置近的代理服务器
- 考虑代理服务器的负载
- 监控代理性能

### 9.4 故障排查注意事项

**日志记录**
- 记录代理配置信息（不含密码）
- 记录代理连接失败的情况
- 便于问题诊断

**测试连接**
```bash
# Test HTTP proxy
curl -v --proxy $BASE_PROXY http://www.example.com

# Test HTTPS proxy
curl -v --proxy $BASE_PROXY https://www.example.com

# Check if proxy is used
curl -v http://www.example.com
```

### 9.5 环境隔离注意事项

**不同环境使用不同代理**
- 开发环境、测试环境、生产环境
- 使用不同的代理配置
- 避免环境混淆

**配置文件管理**
- 使用版本控制管理代理配置模板
- 不要提交包含真实密码的配置
- 使用配置管理工具

---

## 10. 代理故障排查

### 10.1 常见问题

#### 问题1：代理连接失败

**症状**
- 网络请求超时
- 连接被拒绝
- 无法访问外部资源

**排查步骤**
```bash
# 1. Check if proxy environment variables are set
echo $http_proxy
echo $https_proxy

# 2. Test proxy connectivity
curl -v --proxy $http_proxy http://www.example.com

# 3. Check proxy server status
ping proxy.example.com
telnet proxy.example.com 8080

# 4. Verify credentials
curl -v --proxy-user username:password --proxy $http_proxy http://www.example.com
```

**解决方案**
- 检查代理服务器地址和端口
- 验证用户名和密码
- 检查网络连接
- 确认代理服务器运行状态

#### 问题2：某些地址无法访问

**症状**
- 部分网站可以访问，部分不能
- 本地服务无法访问
- 内网资源访问异常

**排查步骤**
```bash
# 1. Check no_proxy configuration
echo $no_proxy

# 2. Test direct connection (bypass proxy)
curl -v --noproxy "*" http://internal.example.com

# 3. Test with proxy
curl -v --proxy $http_proxy http://internal.example.com
```

**解决方案**
- 检查no_proxy配置是否正确
- 确认需要绕过代理的地址已添加
- 验证域名匹配规则

#### 问题3：HTTPS连接失败

**症状**
- HTTP可以访问，HTTPS不行
- SSL证书错误
- 加密连接失败

**排查步骤**
```bash
# 1. Check HTTPS proxy configuration
echo $https_proxy

# 2. Test HTTPS connection
curl -v --proxy $https_proxy https://www.example.com

# 3. Check SSL certificate
openssl s_client -connect proxy.example.com:8080 -showcerts
```

**解决方案**
- 确认HTTPS_PROXY已正确配置
- 检查代理服务器SSL支持
- 验证证书有效性

### 10.2 调试技巧

#### 启用详细日志

```bash
# Enable verbose output for curl
curl -v --proxy $http_proxy http://www.example.com

# Enable debug output for wget
wget --debug --proxy=on http://www.example.com

# Check environment variables
env | grep -i proxy
```

#### 测试代理功能

```bash
# Test script
#!/bin/bash
echo "Testing proxy configuration..."
echo "HTTP_PROXY: $HTTP_PROXY"
echo "HTTPS_PROXY: $HTTPS_PROXY"
echo "NO_PROXY: $NO_PROXY"

echo -e "\nTesting HTTP connection..."
curl -I --proxy $HTTP_PROXY http://www.example.com

echo -e "\nTesting HTTPS connection..."
curl -I --proxy $HTTPS_PROXY https://www.example.com

echo -e "\nTesting no_proxy (should bypass proxy)..."
curl -I --noproxy "*" http://localhost
```

### 10.3 代理性能问题

**症状**
- 网络访问速度慢
- 请求延迟高
- 代理服务器负载高

**排查方法**
```bash
# Measure connection time
time curl --proxy $http_proxy http://www.example.com

# Check proxy server response time
curl -w "@curl-format.txt" --proxy $http_proxy http://www.example.com

# Monitor network traffic
tcpdump -i any -n host proxy.example.com
```

**优化建议**
- 使用地理位置近的代理
- 配置适当的缓存
- 优化no_proxy配置
- 考虑使用多个代理服务器

---

## 11. 代理安全性考虑

### 11.1 认证安全

**密码保护**
- 不要在代码中硬编码密码
- 使用环境变量或密钥管理
- 定期更换密码
- 使用强密码策略

**安全配置示例**
```bash
# Secure way: use credential file with restricted permissions
# chmod 600 ~/.proxy_credentials
source ~/.proxy_credentials

export BASE_PROXY="http://${PROXY_USER}:${PROXY_PASS}@${PROXY_HOST}:${PROXY_PORT}"
```

### 11.2 传输安全

**HTTPS代理**
- 使用HTTPS代理保护传输数据
- 验证代理服务器证书
- 避免中间人攻击

**SOCKS5代理**
- 支持更安全的认证方式
- 提供更好的加密支持

### 11.3 访问控制

**最小权限原则**
- 只授予必要的网络访问权限
- 限制代理使用范围
- 监控代理使用情况

**审计日志**
- 记录所有代理访问
- 分析异常访问模式
- 定期审查访问日志

### 11.4 代理服务器安全

**选择可信代理**
- 使用企业提供的官方代理
- 避免使用不可信的公共代理
- 验证代理服务器身份

**代理服务器配置**
- 确保代理服务器有适当的安全措施
- 定期更新代理服务器软件
- 监控代理服务器安全状态

---

## 12. 总结

### 12.1 关键要点

1. **代理的作用**：代理是客户端和服务器之间的中间层，提供访问控制、安全防护、性能优化等功能。

2. **配置方式**：主要通过环境变量配置，包括`http_proxy`、`https_proxy`和`no_proxy`。

3. **使用场景**：企业内网、开发环境、云服务器、容器化环境等都需要代理配置。

4. **安全考虑**：保护认证信息、使用安全传输、实施访问控制。

5. **故障排查**：通过测试连接、检查配置、查看日志等方式诊断问题。

### 12.2 最佳实践

- ✅ 使用BASE_PROXY变量统一管理代理配置
- ✅ 同时设置大小写版本的环境变量确保兼容性
- ✅ 正确配置no_proxy避免不必要的代理
- ✅ 使用安全的密码管理方式
- ✅ 定期测试代理连接和性能
- ✅ 记录和监控代理使用情况

### 12.3 配置模板

```bash
#!/bin/bash
# Proxy Configuration Template
# Usage: source this file to configure proxy settings

# Proxy server configuration
PROXY_HOST="proxy.example.com"
PROXY_PORT="8080"
PROXY_USER="username"
PROXY_PASS="password"

# Build proxy URL
export BASE_PROXY="http://${PROXY_USER}:${PROXY_PASS}@${PROXY_HOST}:${PROXY_PORT}"

# Standard proxy environment variables
export http_proxy=$BASE_PROXY
export https_proxy=$BASE_PROXY
export HTTP_PROXY=$BASE_PROXY
export HTTPS_PROXY=$BASE_PROXY

# No proxy configuration
export no_proxy="localhost,127.0.0.1,.corp.com,10.0.0.0/8"
export NO_PROXY=$no_proxy

# Verify configuration
echo "Proxy configuration loaded:"
echo "  HTTP_PROXY: $HTTP_PROXY"
echo "  HTTPS_PROXY: $HTTPS_PROXY"
echo "  NO_PROXY: $NO_PROXY"
```

### 12.4 相关资源

- **环境变量文档**：各应用程序的环境变量支持文档
- **代理协议规范**：HTTP代理、SOCKS代理协议规范
- **安全最佳实践**：网络安全和代理安全指南
- **故障排查工具**：curl、wget、tcpdump等网络工具

---

## 附录：常用命令参考

### 检查代理配置
```bash
# Check all proxy-related environment variables
env | grep -i proxy

# Check specific variable
echo $http_proxy
echo $https_proxy
echo $no_proxy
```

### 测试代理连接
```bash
# Test HTTP proxy
curl -v --proxy $http_proxy http://www.example.com

# Test HTTPS proxy
curl -v --proxy $https_proxy https://www.example.com

# Test without proxy
curl -v --noproxy "*" http://www.example.com
```

### 临时启用/禁用代理
```bash
# Enable proxy for current session
export http_proxy="http://proxy.example.com:8080"

# Disable proxy
unset http_proxy
unset https_proxy
unset HTTP_PROXY
unset HTTPS_PROXY
```

### 代理配置验证脚本
```bash
#!/bin/bash
# Proxy configuration verification script

echo "=== Proxy Configuration Check ==="
echo ""

# Check environment variables
echo "Environment Variables:"
[ -z "$http_proxy" ] && echo "  http_proxy: NOT SET" || echo "  http_proxy: $http_proxy"
[ -z "$https_proxy" ] && echo "  https_proxy: NOT SET" || echo "  https_proxy: $https_proxy"
[ -z "$no_proxy" ] && echo "  no_proxy: NOT SET" || echo "  no_proxy: $no_proxy"
echo ""

# Test connectivity
echo "Connectivity Tests:"
if [ -n "$http_proxy" ]; then
    echo -n "  HTTP proxy test: "
    curl -s -o /dev/null -w "%{http_code}" --proxy $http_proxy http://www.example.com && echo " OK" || echo " FAILED"
else
    echo "  HTTP proxy: NOT CONFIGURED"
fi

if [ -n "$https_proxy" ]; then
    echo -n "  HTTPS proxy test: "
    curl -s -o /dev/null -w "%{http_code}" --proxy $https_proxy https://www.example.com && echo " OK" || echo " FAILED"
else
    echo "  HTTPS proxy: NOT CONFIGURED"
fi
```

---

**文档版本**：1.0  
**最后更新**：2024年  
**维护者**：Network Configuration Team

