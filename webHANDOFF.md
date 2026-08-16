# HANDOFF.md — 交接文档

> 写给完全没有上下文的新会话。最后更新：2026-08-13

---

## 一、项目概况

**项目名**：小阁(XiaoGe) — 具身智能语音对话机器人

**两套独立服务，同一台服务器（腾讯云 81.71.24.233 CentOS 7）**：

| 服务 | 框架 | 端口 | Nginx路径 | 用途 |
|---|---|---|---|---|
| Flask server.py | Flask | 5000 | `/api/` | 给ESP32用的语音API（ASR+LLM+TTS） |
| FastAPI robot_admin | FastAPI+Vue3 | 8000 | `/admin/`（前端）`/manage-api/`（API） | Web管理后台 |

**代码位置**：
- 本地管理后台：`C:\Users\33132\Desktop\robot_project`
- 本地ESP32固件：`G:\esp32\esp32_projects\ai_client`
- 服务器Flask：`/home/DC1/local/server.py`
- 服务器FastAPI：`/home/DC1/robot_admin/backend/`
- 服务器前端：`/var/www/robot_admin/`

**服务器凭据**：IP `81.71.24.233`，用户 `DC1`，密码 `ZZC26622@mcz`，sudo需 `echo "password" | sudo -S`

**管理后台登录**：demo/demo123（admin）。管理员DB：`/home/DC1/robot_admin/backend/robot.db`（sqlite）。
**现有用户**：demo(admin)、我要困告了、test、Milke、zbbb（后三者普通用户）。
**环境变量**：
- `voice-assistant.service`：SILICONFLOW_KEY、MINIMAX_API_KEY、BAIDU_API_KEY、BAIDU_SECRET_KEY、LLM_MODEL
- robot-admin 从 `backend/.env` 读（SILICONFLOW_API_KEY、MINIMAX_API_KEY、DB_TYPE 等）

---

## 二、基础功能（早期实现）

### 1. 四个管理功能
- **设备管理** (`/devices`)：CRUD + 心跳自动注册
- **聊天记录** (`/chat-history`)：微信PC风格左右分栏聊天界面
- **AI设置** (`/ai-settings`)：模型选择、系统提示词、temperature、知识库文档上传
- **文档管理** (`/documents`)：文档上传/列表/删除

### 2. LLM 真实接入
- SiliconFlow API（OpenAI兼容），默认 Qwen2.5-7B，可切72B
- API Key 存在服务器 `.env` 的 `SILICONFLOW_API_KEY`
- 用 aiohttp 调用（**不能用httpx，CentOS 7上会卡55秒**）
- Temperature 前后端都 clamp 到 ≤1.0（超过1.0模型会输出乱码）
- max_tokens 后端 clamp 到 ≤500

### 3. 聊天界面
- 微信PC版风格：左侧会话列表，右侧气泡聊天
- 乐观发送 + 发送中禁用发送按钮

### 4. ESP32 设备心跳
- 固件 `heartbeat_task`：WiFi连上后立即发心跳，之后每60秒一次
- device_key = WiFi MAC地址，device_name = "小阁-01"
- 心跳URL：`http://81.71.24.233/manage-api/devices/heartbeat`
- 后端心跳接口自动注册设备

### 5. 子目录部署
- Vite `base: '/admin/'`（生产模式）
- Vue Router 命名路由（不能用路径字符串）
- axios 拦截器 401 跳转用 `BASE_URL + 'login'`

---

## 三、部署流程

```
# 前端构建
cd C:\Users\33132\Desktop\robot_project\frontend
npm run build

# 上传（paramiko SFTP 脚本，见 C:\Users\33132\AppData\Local\Temp\opencode\）
# 1. SFTP上传dist到 ~/robot_admin/frontend/dist_new
# 2. sudo cp -r dist_new/* /var/www/robot_admin/ && sudo chmod -R 755
# 3. 后端.py文件SFTP覆盖到 /home/DC1/robot_admin/backend/app/...
# 4. sudo systemctl restart robot-admin
# 5. Flask: /home/DC1/local/server.py + sudo systemctl restart voice-assistant

# 验证
curl -s -o /dev/null -w "%{http_code}" http://81.71.24.233/admin/
```

**部署脚本都在 `C:\Users\33132\AppData\Local\Temp\opencode\`**，命名如 `deploy_xxx.py`。
**备份约定**：服务器改前 `cp -a ... .bak_xxx`（已有 app.bak_*、server.py.bak_* 系列）。

---

## 四、当前卡在哪 / 未完成

**核心功能已全部完成并上线**，剩余已知限制：

1. **文档上传不支持图片OCR / 旧版 .doc**：仅支持 .txt/.md/.docx/.pdf；.doc 二进制无法提取，图片不能识别。
2. **固件未烧录**：`raw_ws_client.c` 已加 X-Device-Key WS 头并构建通过（`build/AI_client.bin`），
   尚未烧录到 ESP32（烧录前 WS 对话无设备号，归属单台设备）。
3. **MiniMax 费用需关注**：塔菲音色走 MiniMax 按量计费（测试已花 ~10 元），余额用尽时
   塔菲 TTS 会静默失败；克隆音色 7 天不调用会被删除。
4. **Dashboard"系统状态"卡片是硬编码**（显示"MySQL/Redis 连接正常"，实际用 SQLite 无 Redis），
   可改为真实数据（设备在线数/今日对话数/激活人格等）。
5. **用户管理无自助注册页面**：注册接口开放（role=user），但普通用户无法自己成为 admin；
   多账号由 admin 在后台创建或自助注册后由 admin 改角色。

---

## 五、已完成功能（按时间顺序）

### 五之一、知识库RAG + 设备离线检测（2026-08-10）
- `services/rag.py`：extract_text(.txt/.md/.docx/.pdf) → chunk_text(500字/重叠80) →
  embed_texts(BAAI/bge-m3) → retrieve_knowledge(余弦,阈值0.3,top3) → build_knowledge_context。
- 上传文档后台建向量索引；删除文档删向量块；文档列表返回 chunk_count。
- 离线检测：`main.py` lifespan 起 `_offline_sweeper` 每60s 把 last_seen 超3分钟的设备标 offline。
- `retrieve_knowledge` **排除 `[人格-%]` 文档**（避免人格KB残留串台）。

### 五之二、聊天记录设备端语音上报（2026-08-10）
- Flask `save_voice_chat()` 写 chat_messages（source='voice'），并读取当前 persona 一起写入。
- WS 流式 end 分支 + HTTP /api/asr 兜底都异步调用；WS 握手读 X-Device-Key 头。
- 固件 raw_ws_client.c 加 X-Device-Key 头（已构建未烧录）。

### 五之三、预置人格系统（奶龙/塔菲/自定义）（2026-08-10~11）
- `services/personas.py`：奶龙(CosyVoice2 bella + edge-tts Xiaoyi)、塔菲(minimax克隆音色1.25x)、自定义。
- 人格切换 `POST /personas/{id}/apply`：预置人格写 system_prompt/音色/模型 + 重建 KB `[人格-{id}].txt`；
  自定义仅清 persona 标记（不覆盖用户配置）+ 删人格KB。
- `_active_ai_settings()` 动态作用于语音；`tts_generate` 三引擎分流（minimax:/speech:/edge-tts）。
- 前端 AISettings.vue 人格卡片、自定义头像上传、固定人格锁定 prompt+temperature。

### 五之四、人格记忆分离 + 聊天记录按人格过滤（2026-08-11）
- chat_messages 加 persona 列；新增 persona_memories 表（各人格独立记忆）。
- send_message 带 persona；call_llm 读当前人格记忆；_update_memory 按人格；list_messages 支持 persona 过滤。
- 前端 ChatHistory 左侧四会话切换（奶龙/塔菲/自定义/全部）。

### 五之五、MiniMax 塔菲音色（2026-08-11）
- 平台 `https://api.minimaxi.com`（国内站）。需**实名认证**（2038）+ **充值有余额**（1008）。
- 克隆：files/upload(voice_clone 28s + prompt_audio 6s) → voice_clone（voice_id=tafei_voice_001）。
- TTS：`POST /v1/t2a_v2`，`data.audio` 为 **hex 编码**需 `bytes.fromhex()`。
- 塔菲 `minimax:tafei_voice_001@1.25`；**≤7字短文本合成异常** → 试听自动补长文本。

### 五之六、自定义人格提示词/头像隔离 + 塔菲喵句尾（2026-08-11）
- ai_settings 加 `custom_prompt`（自定义人格专用提示词）、`custom_icon`（base64头像）。
- `_ensure_tail_miao()`：塔菲回复确定性后处理，每句末尾补"喵"（先删喵→按`。！？`切句→补尾）。
- `POST /ai-settings/custom-icon` 上传头像；ChatHistory 自定义会话头像联动。

### 五之七、用户管理 + 操作日志 + 文档按人格隔离 + 聊天分页/清空（2026-08-13）
- **用户管理**：User 加 role(admin/user)，`get_admin_user` 依赖（非admin 403），`api/users.py`
  （列表/创建/改角色/重置密码/删除，保护最后一个admin），demo 自动为 admin。
- **操作日志**：operation_logs 表 + `services/logger.py log_action()`；埋点：apply_persona/
  upload_doc/delete_doc/clear_chat/delete_chat/user_manage；`api/logs.py`（admin only，X-Total-Count）。
- **文档按人格隔离**：knowledge_documents/chunks 加 persona 列；list/upload 支持 persona；
  retrieve_knowledge 按 `(persona IS NULL) OR (persona==当前)` 过滤。
- **聊天分页/清空**：前端滚动加载；`DELETE /chat/persona/{persona}` 清空该人格聊天+记忆
  （路由须在 `/{message_id}` 之前）。
- **AI设置保存后设备重启提醒**。

### 五之八、聊天归属/音色同步/会话预览/时区 修复（2026-08-13）
1. **聊天归属**：`ChatSend` 加 persona，send_message 用前端选中会话归属消息 +
   call_llm 用该人格 prompt/记忆/知识库；list_messages 的 custom 匹配 null/'custom'；
   前端 personaMatches()。
2. **音色同步**：apply custom 时 `tts_voice_edge = tts_voice`；前端 handleSave 自定义同步。
3. **会话预览**：`previewMessages` 并行拉全量供左侧列表预览。
4. **时区**：SQLite `func.now()` 存 UTC 但序列化无时区 → 新增 `schemas/base.py`
   `UTCDateTimeModel`（field_validator("*") 标记 UTC），所有响应 schema 继承，
   输出带 `Z`，前端 new Date() 自动转本地。**修复了所有页面差8小时问题**。

### 五之九、提示词写作指南（2026-08-13）
- `frontend/public/prompt-guide.html` → `/admin/prompt-guide.html`。
- AI设置页"AI 人格"卡片右上角"📖 提示词写作指南"链接。
- 内容：5大必备部分、7条写作技巧、完整范例、雷区、测试方法。

---

## 六、踩过的坑（绝对不要再踩）

### Python / 后端
| 坑 | 原因 | 解法 |
|---|---|---|
| `dict \| None` 语法报错 | 服务器Python 3.9 | 用 `Optional[dict]` |
| httpx调SiliconFlow卡55秒 | CentOS 7上httpx bug | **用aiohttp** |
| `resp.json()` 中文乱码 | aiohttp编码检测错 | `await resp.text(encoding='utf-8')` 再 json.loads |
| SQLite不接受pool_size | pool参数MySQL专用 | sqlite 不传 |
| Temperature设到2.0 | 输出乱码 | 前后端clamp到≤1.0 |
| 离线测试缺SILICONFLOW_KEY/MINIMAX_API_KEY | 只有systemd有 | `os.environ.setdefault(...)` |
| 语音侧短模型名报'choices' | Flask直发短名 | llm_chat加model_map映射完整ID |
| MiniMax短文本合成异常 | ≤7字只返回~0.3s | 试听/合成前自动补长文本 |
| MiniMax克隆WAV头异常 | 返回0xFFFFFFFF | ffmpeg重编码修复 |
| 句末补字`……`切分出错 | 省略号被拆碎 | 只用`。！？`切分，省略号rstrip处理 |
| axios FormData没设content-type | 全局默认application/json | 显式设multipart/form-data |
| RAG注入残留人格KB | 已删文档chunk仍被检索 | retrieve_knowledge join排除`[人格-%]` |
| persona=None判断失效 | `if persona and ...`对None不生效 | 改`if not persona or ...` |
| `await db.execute(...).scalar_one_or_none()`报错 | await优先级 | 加括号`(await db.execute(...)).scalar_one_or_none()` |
| 路由`/persona/{x}`被`/{id}`匹配 | FastAPI按注册顺序 | 特例路由放`/{id}`之前 |
| 消息"瞬间消失"只有"全部"能看到 | send_message用DB激活人格而非前端会话 | ChatSend加persona，前端传activePersona |
| 自定义人格设备端音色错误 | 只存tts_voice，edge残留上一人格 | apply custom时edge=voice；前端同步 |
| 时间显示差8小时 | SQLite func.now()存UTC但序列化无时区 | UTCDateTimeModel基类标记UTC输出带Z |

### MiniMax
| 坑 | 原因 | 解法 |
|---|---|---|
| api.minimax.io key无效 | 国内站才是api.minimaxi.com | 用 `https://api.minimaxi.com` |
| 克隆报2038 | 未实名认证 | platform.minimaxi.com 实名 |
| TTS报1008/2056 | 余额不足 / Token Plan上限 | 按量key(sk-api)充值；sk-cp是订阅 |
| 扣费快 | 按量计费 | 塔菲用MiniMax，其他用免费；可买语音资源包¥400/200万字符 |

### 前端 / 部署
| 坑 | 原因 | 解法 |
|---|---|---|
| Vue Router push('/login') 404 | 子目录部署 | 用命名路由 |
| Git Bash把/admin/转Windows路径 | 路径转换 | Vite base用mode判断 |
| Nginx 500 Permission denied | /home/DC1权限700 | 前端放/var/www/robot_admin/ |
| 浏览器缓存旧文件 | 强缓存 | Ctrl+Shift+R |

### 服务器
| 坑 | 原因 | 解法 |
|---|---|---|
| sudo在paramiko卡住 | 无tty | `echo pass \| sudo -S bash -c 'cmd'` |
| systemd没miniconda PATH | 未配 | Environment=PATH=... |
| /api/被Flask占用 | 语音API | FastAPI走/manage-api/ |

---

## 七、关键文件速查

```
robot_project/backend/app/
├── api/chat.py          # 聊天 + LLM + 按人格记忆 + 塔菲喵 + 清空该人格
├── api/ai_settings.py   # AI设置 + 人格 + 知识库(按人格) + 试听 + 自定义头像
├── api/users.py         # 用户管理（admin only）
├── api/logs.py          # 操作日志（admin only，X-Total-Count）
├── api/devices.py       # 设备CRUD + 心跳自动注册
├── api/auth.py          # 注册/登录/登出/me（注册默认 role=user）
├── models/ai_settings.py # AISettings + KnowledgeDocument/Chunk(persona)
├── models/chat.py       # ChatMessage(persona) + PersonaMemory
├── models/operation_log.py # 操作日志表
├── models/user.py       # User（role 字段）
├── services/personas.py # 奶龙/塔菲/自定义人格定义
├── services/rag.py      # RAG（排除人格KB + 按persona过滤）
├── services/logger.py   # log_action() 操作日志
├── core/deps.py         # get_current_user + get_admin_user
├── schemas/             # Pydantic（base.py = UTCDateTimeModel 时区基类）
├── config.py            # .env读取（SILICONFLOW/MINIMAX）
├── main.py              # 迁移 + 离线sweeper + 路由注册

robot_project/frontend/src/
├── views/AISettings.vue # AI设置 + 人格卡片 + 自定义头像 + 保存提醒 + 指南链接
├── views/ChatHistory.vue # 聊天页（按人格会话 + 分页 + 清空 + 预览）
├── views/Devices.vue    # 设备管理
├── views/Documents.vue  # 文档管理（按人格筛选）
├── views/Users.vue      # 用户管理（admin）
├── views/Logs.vue       # 操作日志（admin）
├── api/aiSettings.ts / chat.ts / users.ts / logs.ts / auth.ts
└── public/prompt-guide.html  # 提示词写作指南

服务器：
- /home/DC1/local/server.py          # Flask语音服务
- /home/DC1/robot_admin/backend/     # FastAPI
- /var/www/robot_admin/              # 前端dist
- 备份：server.py.bak_*、app.bak_* 系列

本地素材：
- C:\Users\33132\Desktop\robot_project\voice_samples\   # 塔菲音频相关
- 奶龙头像源：A1B3D12E2F684660D7F68EE62B8C2AD5.png
- 塔菲头像源：23898A64A2A83DED2AD19346B354DDDD.png
```
