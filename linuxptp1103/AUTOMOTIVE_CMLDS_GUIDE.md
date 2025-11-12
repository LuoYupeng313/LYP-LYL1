# IEEE 802.1AS Automotive Profile with CMLDS and gPTP-capable TLV

## 架构说明

### CMLDS (Common Mean Link Delay Service) 机制

```
                    网络拓扑
                         
    ┌─────────────────────────────────┐
    │    Marvell Switch (Bridge)      │
    │    需要接收gPTP-capable TLV      │
    └────────────┬────────────────────┘
                 │
                 │ Sync/Follow_Up (Domain 0 & 1)
                 │
    ┌────────────▼────────────────────┐
    │      Your End Station           │
    │                                 │
    │  ┌──────────────────────┐       │
    │  │   CMLDS Server       │       │
    │  │   (独立进程)          │       │
    │  │   Domain: 0          │       │
    │  │   transportSpecific  │       │
    │  │   = 0x2              │       │
    │  │   负责P2P延迟计算     │       │
    │  └─────────┬────────────┘       │
    │            │ UDS Socket         │
    │            │ /var/run/cmlds_*   │
    │       ┌────┴─────┐              │
    │       │          │              │
    │  ┌────▼──────┐  ┌▼───────────┐ │
    │  │ ptp4l     │  │ ptp4l      │ │
    │  │ Domain 0  │  │ Domain 1   │ │
    │  │ (Master)  │  │ (Slave)    │ │
    │  │ serverOnly│  │ clientOnly │ │
    │  │           │  │            │ │
    │  │ 发送Sync  │  │ 接收Sync   │ │
    │  └───────────┘  └────────────┘ │
    └─────────────────────────────────┘
```

### 关键概念

1. **CMLDS Server**
   - 独立的PTP进程
   - 负责执行peer delay测量（P2P机制）
   - 通过UDS socket与ptp4l实例通信
   - 提供meanLinkDelay给其他实例

2. **Domain 0 (Master)**
   - `serverOnly = 1`: 只作为时间服务器
   - `inhibit_announce = 1`: 不发送Announce
   - `BMCA = noop`: 不参与BMCA选举
   - 从Domain 1同步，向下游发送Sync

3. **Domain 1 (Slave)**
   - `clientOnly = 1`: 只作为时间客户端
   - 从上游Master（Marvell交换机）接收Sync
   - 同步本地时钟

4. **delay_mechanism = COMMON_P2P**
   - 延迟计算由CMLDS统一处理
   - ptp4l实例从CMLDS获取延迟信息
   - 避免重复的peer delay测量

---

## gPTP-capable TLV (0x8000) 发送规则

### ❓ Master需要发送吗？

**答案：是的，Master和Slave都应该发送！**

| 角色 | 发送0x8000 | 原因 |
|------|-----------|------|
| **CMLDS Server** | ✅ **应该发送** | 向网络通告：我负责延迟测量 |
| **Domain 0 Master** | ✅ **必须发送** | 向下游（如果有）和交换机通告能力 |
| **Domain 1 Slave** | ✅ **必须发送** | 向上游（交换机）通告能力，**这是关键！** |

### 为什么Master也要发送？

1. **双向通告机制**
   - 0x8000不是请求-响应模型
   - 而是**单向能力通告**："我支持gPTP"
   - 每个端口独立通告自己的能力

2. **网络拓扑发现**
   - 交换机需要知道哪些端口是gPTP-capable
   - Master端口也需要被交换机识别
   - 用于正确的帧转发和时间戳插入

3. **Bridge行为**
   - 如果将来您的设备作为Bridge（中继）
   - 两个方向的端口都需要通告能力
   - 确保双向时间同步路径畅通

---

## 配置文件

### 1. CMLDS Server配置

**文件：`CMLDS_server.cfg`**

```ini
[global]
clientOnly              1
clockIdentity           C37D50.0000.000000
free_running            1
ignore_transport_specific 1
transportSpecific       2          # Automotive Profile
uds_address             /var/run/cmlds_server
network_transport       L2
domainNumber            0

# 启用gPTP-capable TLV
gptp_capable_transmit   1

[ens33]
delay_mechanism         P2P
```

### 2. Domain 0 Master配置

**文件：`domain0_master_gptp.cfg`**

```ini
[global]
domainNumber            0
gmCapable               1
priority1               248
priority2               248
logSyncInterval         -3
transportSpecific       0x2        # Automotive Profile
network_transport       L2
delay_mechanism         COMMON_P2P # 使用CMLDS

BMCA                    noop
serverOnly              1
inhibit_announce        1
asCapable               true

# *** CRITICAL: Master也要发送 ***
gptp_capable_transmit   1

[ens33]
cmlds.server_address    /var/run/cmlds_server
cmlds.port              0
cmlds.domainNumber      0
```

### 3. Domain 1 Slave配置

**文件：`domain1_slave_gptp.cfg`**

```ini
[global]
domainNumber            1
gmCapable               1
priority1               248
priority2               248
logSyncInterval         -3
transportSpecific       0x2        # Automotive Profile
network_transport       L2
delay_mechanism         COMMON_P2P # 使用CMLDS

BMCA                    noop
clientOnly              1
inhibit_announce        1
asCapable               true

# *** CRITICAL: Slave必须发送 ***
gptp_capable_transmit   1

[ens33]
cmlds.server_address    /var/run/cmlds_server
cmlds.port              0
cmlds.domainNumber      0
```

---

## 启动顺序

### 完整启动流程

```bash
# 1. 首先启动CMLDS Server
ptp4l -f CMLDS_server.cfg -i ens33 -m &
sleep 2

# 2. 启动Domain 0 Master (会发送gPTP-capable TLV)
ptp4l -f domain0_master_gptp.cfg -i ens33 -m &
sleep 2

# 3. 启动Domain 1 Slave (会发送gPTP-capable TLV)
ptp4l -f domain1_slave_gptp.cfg -i ens33 -m &
```

### 期望的日志输出

```
# CMLDS Server
ptp4l[xxx]: port 1 (ens33): sent gPTP-capable signaling message
ptp4l[xxx]: port 1 (ens33): LISTENING to UNCALIBRATED

# Domain 0 Master
ptp4l[xxx]: port 1 (ens33): sent gPTP-capable signaling message
ptp4l[xxx]: port 1 (ens33): LISTENING to MASTER

# Domain 1 Slave
ptp4l[xxx]: port 1 (ens33): sent gPTP-capable signaling message
ptp4l[xxx]: port 1 (ens33): LISTENING to UNCALIBRATED
ptp4l[xxx]: port 1 (ens33): UNCALIBRATED to SLAVE
ptp4l[xxx]: rms   15 max   28   freq  -1234 +/-  25   offset    8 +/-  12
```

---

## transportSpecific 差异

### 0x1 vs 0x2

| 值 | 标准 | 使用场景 |
|----|------|---------|
| **0x1** | IEEE 802.1AS-2011/2020 | 标准gPTP，工业以太网 |
| **0x2** | Automotive Profile | 汽车以太网，CMLDS架构 |

**重要**：两者都支持gPTP-capable TLV (0x8000)！

我的实现已修正，现在支持：
```c
// 支持 0x1 (标准gPTP) 和 0x2 (Automotive Profile)
if (!port_is_ieee8021as(p) && p->transportSpecific != (2 << 4)) {
    return 0;  // 只有非802.1AS相关的才跳过
}
```

---

## 与Marvell交换机的交互

### 交换机视角

```
端节点                          Marvell交换机
  │                                  │
  │ 1. Peer Delay Req ────────────> │
  │ <──────────── Peer Delay Resp   │
  │                                  │
  │ 2. gPTP-capable Signaling (0x8000) ──> │
  │    TLV内容：                     │
  │    - 我支持gPTP                   │
  │    - asCapable = true            │
  │                                  │
  │              交换机更新端口状态:   │
  │              "此端口gPTP-capable" │
  │                                  │
  │ 3. <────────── Sync Messages     │  ✅ 现在会转发！
  │    <────────── Follow_Up         │
```

### 没有0x8000 TLV的后果

```
端节点                          Marvell交换机
  │                                  │
  │ Peer Delay Req ────────────────> │
  │ <──────────── Peer Delay Resp   │
  │                                  │
  │ ❌ 没有发送gPTP-capable TLV      │
  │                                  │
  │              交换机判断:          │
  │              "此端口不支持gPTP"   │
  │              "不转发Sync到此端口" │
  │                                  │
  │ ❌ Sync Messages (被丢弃)        │  ⛔ 不转发
  │                                  │
  └─ 无法同步，一直处于LISTENING状态  │
```

---

## 验证方法

### 1. 检查所有进程都发送了TLV

```bash
# 检查CMLDS Server
grep "sent gPTP-capable" /var/log/syslog | grep "cmlds"

# 检查Domain 0 Master
grep "sent gPTP-capable" /var/log/syslog | grep "domain 0"

# 检查Domain 1 Slave
grep "sent gPTP-capable" /var/log/syslog | grep "domain 1"
```

### 2. 网络抓包

```bash
tcpdump -i ens33 -vvv ether proto 0x88f7 and ether dst 01:80:C2:00:00:0E
```

应该看到多个源MAC地址发送的Signaling报文，包含TLV 0x8000。

### 3. 验证asCapable状态

```bash
# 检查CMLDS
pmc -u -b 0 -d 0 -t 2 'GET PORT_DATA_SET_NP'

# 检查Domain 0
pmc -u -b 0 -d 0 'GET PORT_DATA_SET_NP'

# 检查Domain 1
pmc -u -b 0 -d 1 'GET PORT_DATA_SET_NP'
```

都应该显示 `asCapable: 1`

### 4. 检查同步状态

Domain 1 Slave应该显示：
```
port 1 (ens33): SLAVE
rms   15 max   28   freq  -1234 +/-  25   offset    8 +/-  12
```

---

## 故障排查

### 问题：Domain 1 Slave仍无法同步

**检查清单**：

1. ✅ **所有三个进程都启动了吗？**
   ```bash
   ps aux | grep ptp4l
   # 应该看到3个ptp4l进程
   ```

2. ✅ **所有进程都发送了gPTP-capable TLV吗？**
   ```bash
   grep "sent gPTP-capable" /var/log/syslog
   ```

3. ✅ **CMLDS socket通信正常吗？**
   ```bash
   ls -la /var/run/cmlds_*
   # 应该存在 cmlds_server 和 cmlds_client
   ```

4. ✅ **transportSpecific配置正确吗？**
   - CMLDS Server: `transportSpecific = 2`
   - Domain 0/1: `transportSpecific = 0x2`

5. ✅ **Marvell交换机PTP已启用吗？**
   - 登录交换机查看PTP配置
   - 确认端口PTP功能启用

### 问题：Master发送TLV但无效果

**原因**：Master发送是正确的，但对**Slave能否接收Sync**影响较小。

**关键是Slave的TLV**：
- Slave的0x8000 TLV告诉交换机"请转发Sync给我"
- Master的0x8000 TLV告诉交换机"我的端口支持gPTP"
- 两者都重要，但Slave的更关键

---

## 总结

### 关键要点

1. ✅ **Master和Slave都应该发送0x8000 TLV**
2. ✅ **Automotive Profile (0x2) 也需要此TLV**
3. ✅ **CMLDS架构下，三个进程都应配置 `gptp_capable_transmit = 1`**
4. ✅ **0x8000是单向通告，不是请求-响应**
5. ✅ **对于Marvell交换机，Slave的TLV最关键**

### 配置模板

**最小化配置**：在您现有配置文件的`[global]`段添加：

```ini
# 对于所有配置文件（CMLDS_server.cfg, domain0_master.cfg, domain1_slave.cfg）
gptp_capable_transmit   1
```

### 下一步

1. 在三个配置文件中都添加 `gptp_capable_transmit = 1`
2. 重启所有PTP进程
3. 验证都发送了gPTP-capable TLV
4. 观察Domain 1 Slave是否能同步

---

**现在您应该能够成功与Marvell交换机同步了！** 🎉

