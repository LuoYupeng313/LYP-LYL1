# 快速参考：gPTP-capable TLV 配置

## 一句话总结

**Master和Slave都必须发送0x8000 TLV，在三个配置文件中都加上 `gptp_capable_transmit = 1`**

---

## 您的CMLDS架构配置

### 修改三个文件

#### 1. `CMLDS_server.cfg`
在 `[global]` 段添加：
```ini
gptp_capable_transmit   1
```

#### 2. `domain0_master.cfg`  
在 `[global]` 段添加：
```ini
gptp_capable_transmit   1
```

#### 3. `domain1_slave.cfg`
在 `[global]` 段添加：
```ini
gptp_capable_transmit   1
```

---

## 为什么Master也要发送？

| 问题 | 答案 |
|------|------|
| 0x8000是请求吗？ | ❌ 不是，是**单向能力通告** |
| 只有Slave需要吗？ | ❌ Master和Slave都需要 |
| 影响同步吗？ | ✅ Slave的最关键，Master的也重要 |
| CMLDS需要吗？ | ✅ 也建议发送 |

---

## 快速启动

```bash
# 1. 修改配置文件（添加 gptp_capable_transmit = 1）

# 2. 启动
ptp4l -f CMLDS_server.cfg -i ens33 -m &
sleep 2
ptp4l -f domain0_master.cfg -i ens33 -m &
sleep 2
ptp4l -f domain1_slave.cfg -i ens33 -m &

# 3. 验证
grep "sent gPTP-capable" /var/log/syslog
```

---

## 验证成功的标志

```
✅ 日志中看到三次 "sent gPTP-capable signaling message"
✅ Domain 1 进入 SLAVE 状态
✅ 看到 "rms xxx max xxx freq xxx offset xxx" 输出
```

---

## 与Marvell交换机同步失败的根本原因

```
原因：缺少0x8000 TLV
↓
Marvell交换机认为端口不支持gPTP
↓
交换机不转发Sync报文到该端口
↓
端节点无法同步
```

**解决**：发送0x8000 TLV通告能力 ✅

---

## transportSpecific说明

| 您的配置 | 值 | 说明 |
|---------|-----|------|
| CMLDS_server.cfg | `transportSpecific 2` | Automotive Profile |
| domain0_master.cfg | `transportSpecific 0x2` | Automotive Profile |
| domain1_slave.cfg | `transportSpecific 0x2` | Automotive Profile |

**都支持0x8000 TLV！** 代码已适配。

---

## 配置优先级

**最关键**：Domain 1 Slave的 `gptp_capable_transmit = 1`  
**重要**：Domain 0 Master的 `gptp_capable_transmit = 1`  
**建议**：CMLDS Server的 `gptp_capable_transmit = 1`

**建议全部启用！**

