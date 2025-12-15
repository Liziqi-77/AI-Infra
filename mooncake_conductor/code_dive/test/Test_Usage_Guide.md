# Go 语言单元测试使用指南 (针对 KV Event Manager)

本文档旨在帮助初学者快速上手 Go 语言的单元测试，并验证 `StaticManager` 的功能。

## 1. 基础概念

在 Go 语言中：
*   测试文件必须以 `_test.go` 结尾（例如 `static_manager_test.go`）。
*   测试函数必须以 `Test` 开头，并接收 `*testing.T` 参数（例如 `func TestStaticManager_Lifecycle(t *testing.T)`）。
*   运行测试使用 `go test` 命令。

## 2. 环境准备

由于我们的代码使用了 `//go:build zmq` 构建标签（Build Tag），**必须**在运行测试时显式启用该标签，否则编译器会忽略这些文件。

### VS Code / Cursor 配置 (推荐)
为了让编辑器正确识别代码并消除 "No packages found" 警告，请按以下步骤配置：

1.  打开设置 (Ctrl+,)。
2.  搜索 `Go: Build Flags` 或 `gopls.buildFlags`。
3.  添加一个项：`-tags=zmq`。
4.  重新加载窗口 (Ctrl+Shift+P -> `Developer: Reload Window`)。

## 3. 运行测试

### 方法一：命令行运行 (最稳妥)

打开终端，进入项目根目录，执行以下命令：

```bash
# 1. 切换到 kvevent 包目录
cd pkg/kvevent

# 2. 运行该目录下的所有测试
# -tags=zmq: 启用 ZMQ 功能
# -v: 显示详细输出 (Verbose)
go test -tags=zmq -v
```

**预期输出**：
```text
=== RUN   TestStaticManager_Lifecycle
    static_manager_test.go:70: Starting Manager...
    static_manager_test.go:77: Subscriber active for: test-vllm-01
    static_manager_test.go:77: Subscriber active for: test-mooncake-01
    static_manager_test.go:92: Stopping Manager...
    static_manager_test.go:96: Manager lifecycle test passed
--- PASS: TestStaticManager_Lifecycle (0.11s)
=== RUN   TestStaticManager_SubscribeIdempotency
--- PASS: TestStaticManager_SubscribeIdempotency (0.00s)
PASS
ok      github.com/vllm-project/aibrix/pkg/kvevent      0.358s
```

### 方法二：运行特定测试用例

如果您只想运行某一个特定的测试函数（例如 `TestStaticManager_Lifecycle`）：

```bash
go test -tags=zmq -v -run TestStaticManager_Lifecycle
```

### 方法三：查看测试覆盖率

查看代码有多少行被测试覆盖到了：

```bash
# 生成覆盖率文件 cover.out
go test -tags=zmq -coverprofile=cover.out

# 在浏览器中查看可视化的覆盖率报告
go tool cover -html=cover.out
```

## 4. 测试代码解析

打开 `pkg/kvevent/static_manager_test.go`，我们可以看到：

### 4.1 Mock 对象 (模拟依赖)
由于单元测试不应该依赖外部真实的 Redis 或 ZMQ 服务，我们创建了 `MockSyncProvider` 和 `MockSyncIndexer`。
*   **作用**：假装自己是真实的组件，接收调用并返回成功，或者记录调用日志。

```go
type MockSyncProvider struct{}

func (m *MockSyncProvider) GetSyncIndexer(ctx context.Context) (SyncIndexer, error) {
    // 返回一个假 Indexer，而不是去连接 Redis
    return &MockSyncIndexer{}, nil
}
```

### 4.2 测试逻辑
`TestStaticManager_Lifecycle` 模拟了完整的使用流程：

1.  **准备数据**：构造两个假的 ServiceConfig。
2.  **创建管理器**：`NewStaticManager(...)`。
3.  **启动**：`manager.Start()`。这里虽然会尝试连接 `127.0.0.1:55557`，但 ZMQ 的连接是异步的，即使没有服务监听也不会报错 Panic，这正是我们想要的——验证**流程**是否通畅。
4.  **验证状态**：检查 `manager.subscribers` 里是不是真的有两个客户端。
5.  **清理**：调用 `manager.Stop()`。

## 5. 常见问题排查

### Q1: `build constraints exclude all Go files`
**原因**：您没有加 `-tags=zmq`。
**解决**：请务必在命令中加上该参数。

### Q2: `undefined: NewStaticManager`
**原因**：同上，编译器忽略了 `static_manager.go` 文件，导致找不到函数定义。

### Q3: 依赖包缺失 (`no required module provides package`)
**原因**：缺少第三方库。
**解决**：在项目根目录运行 `go mod tidy` 下载所有依赖。

### Q4: 测试卡住不动
**原因**：可能是 `Start()` 中的 WaitGroup 没有正确 Done，或者是 ZMQ 库在某些特殊网络环境下阻塞。
**解决**：
1.  按 `Ctrl+C` 终止。
2.  检查代码中是否有死锁。
3.  在测试命令加 `-timeout 30s` 强制超时。

