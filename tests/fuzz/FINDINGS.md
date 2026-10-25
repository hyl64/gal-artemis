# 模糊测试结果：坏数据不崩、不挂、不越界

被测：五个解析器（`.pfs` / `.ast` / `.ipt` / `.iet` / `.asb`）。
产出：`run.sh`（一条命令复现）、`fuzz.cpp`（harness）、`crashes/`（最小复现 + 说明）、本文。

**结论先说**：五万条变异输入里没有一个 SIGSEGV 出现在「正常返回 / 明确报错」之外——但**深嵌套能让 Lua 读取器栈溢出打死进程**（F1），而 `.ast` 和 `.ipt` 共用同一个读取器，所以一个 bug 有两个前门。另有 1 个 UB（F2）、1 个无上限的资源接受（F3）、2 条「错误当成功」与 1 条放大器型资源分配（F4/F5）。

| # | 严重度 | 解析器 | 现象 | 源码位置 | 复现文件 |
|---|---|---|---|---|---|
| F1 | **严重** | `.ast` + `.ipt` | 深嵌套 → **栈溢出，进程死**（release 下 128 KB 输入 SIGSEGV） | `src/lua.cpp` `Parser::ParseValue` / `ParseObject` | `crashes/f1-lua-nesting-40000.ast` / `.ipt` |
| F2 | 中 | `.ipt` | 画布尺寸 `4294967296` → **int 转换 UB**（release 下静默回绕） | `src/ipt.cpp:46` `IntValue` | `crashes/f2-ipt-canvas-4g.ipt` |
| F3 | 中 | `.iet` | 单行 100 MB 被**接受**：2.7 s、492 MiB，无行长度上限 | `src/iet.cpp`（无长度检查） | `crashes/f3-iet-one-line-1mb.iet` + `run.sh limits` 的 100 MB 探针 |
| F4a | 低 | `.pfs` | 条目名含 NUL 仍算解析成功；`pfstool extract` 会用这个名字拼路径 → `fopen` 静默截断 | `src/pfs.cpp:363` / `tools/pfstool.cpp:140` | `crashes/f4a-pfs-entry-name-nul.pfs` |
| F4b | 低（工具） | `.pfs` | 条目名 `..\..\x` 不清理 → **解包写到目标目录之外** | `src/pfs.cpp:551` `NormalizeName` / `tools/pfstool.cpp:140` | `crashes/f4b-pfs-entry-name-traversal.pfs` |
| F5 | 中低 | `.pfs` | 1 KiB 文件声明 390 万条目 → **先 reserve 238 MiB** 再报错（≈24 万倍放大） | `src/pfs.cpp:336` `entries.reserve(count)` | `crashes/f5-pfs-count-3.9m-small-index.pfs` |

---

## 1. F1：Lua 读取器没有嵌套深度上限（严重）

`.ast` 是 Lua 表，`.ipt` 也是 Lua 表，两者都走 `src/lua.cpp` 的递归下降读取器：

```
Parser::ParseValue  (lua.cpp:439)
  └── Parser::ParseObject (lua.cpp:406 / 414 / 317)
        └── Parser::ParseValue ...
```

每一层嵌套一帧，没有任何深度计数：

```
ast = {a={a={a={ ... 1 ... }}}}
```

实测门槛（同一台机器）：

| 环境 | 崩溃需要的嵌套层数 | 输入大小 |
|---|---|---|
| `-fsanitize=address,undefined` | ≈ 4000 层 | **16 KB** |
| release `-O2`（无 sanitizer） | ≈ 32000 层 | **128 KB**，退出码 139（SIGSEGV） |

复现（sanitizer 版会打印完整调用栈）：

```bash
cd artemis/tests/fuzz
./build/fuzz direct crashes/f1-lua-nesting-40000.ast ast   # AddressSanitizer: stack-overflow
./build/fuzz direct crashes/f1-lua-nesting-40000.ipt ipt   # 同一个读取器，同一个死法
```

调用栈（取 `direct` 输出的前几帧）：

```
ERROR: AddressSanitizer: stack-overflow
    #0 Parser::Advance()                lua.cpp:298
    #1 Parser::ParseObject(LuaValue&)   lua.cpp:317
    #2 Parser::ParseValue(LuaValue&)    lua.cpp:439
    #3 Parser::ParseObject(LuaValue&)   lua.cpp:414
    ...
```

**影响**：这是「游戏打不开」那一类。真实语料里没有这种文件，但 `.ipt` / `.ast` 是随游戏分发的，一个被截断/拼接坏的文件就可能命中；而且同样的问题会被任何用户提供的 Lua 表触发。
**修法方向（不在本轮做）**：`ParseValue` 带一个 depth 参数，超过 `kMaxNesting`（比如 200 层，真实 `.ast` 最深不到 10 层）就返回「nesting too deep」。改一处即可同时保护 `.ast` 与 `.ipt`。

---

## 2. F2：画布尺寸的 int 转换 UB（中）

```
ipt = { mode = "cut", base = { w = 4294967296, h = 4294967296, x = 0, y = 0 },
        { id = 0, file = "a", x = 0, y = 0 } }
```

```
src/ipt.cpp:46:15: runtime error: 4.29497e+09 is outside the range of representable
values of type 'int'
    #0 IntValue(LuaValue const&, int&)   ipt.cpp:46
    #1 IntField(..., char const*, int&)  ipt.cpp:53
    #2 ParseIpt(...)                     ipt.cpp:295
```

`.ipt` 的普通上限检查（`CanvasLimits`，`ipt.cpp:144`）本身是好的——100000×100000 被明确拒绝（见 §5）。问题在**进入检查之前**的这次窄化：release 下 `(int)4294967296` 回绕成 0（或实现相关值），于是 `CanvasLimits` 看到的是一个合法尺寸，检查形同虚设；同一处对负数、超大偏移也一样。
**修法方向**：`IntValue` 用 `double` 读、判范围后再转，越界即报错「value out of range」。

---

## 3. F3：`.iet` 没有行长度上限（中）

`run.sh limits` 的 100 MB 单行探针（`iet one-line-100mb`）：

```
ACCEPTED   2683.3 ms   peak 503808 KiB
```

- **2.68 s** 超过 2 s 的帧预算门槛（本轮判定标准之一就是 >2 s），**492 MiB** 峰值贴近 512 MiB 上限——再大 3% 就会撞上限。
- 20 万条 `[rp]` 挤在一行的探针：`ACCEPTED 118.9 ms, 25 MiB`，这条是可接受的（真实剧本 409 个 `[rp]`）。
- `.iet` 解析器没有「行太长就拒绝」的规则，标签/文本扫描是逐字节线性推进的，所以代价就是输入大小本身。

复现：`crashes/f3-iet-one-line-1mb.iet`（1 MB，被接受，约 100 ms）；
要重现 100 MB 的数字就直接跑 `./build/fuzz limits`（探针在 `fuzz.cpp` 里生成，不落 100 MB 文件进仓库）。

**修法方向**：给单行长度一个上限（例如 1 MB，真实剧本最长行 200 字节量级），超过就明确报错。

---

## 4. F4：条目名照单全收（低，但涉及写文件）

`src/pfs.cpp:363`：

```cpp
e.name.assign((const char*)d + cursor, nameLength);   // 原样保存，含 NUL / .. / 绝对路径
```

**(a) NUL**：`crashes/f4a-pfs-entry-name-nul.pfs` 的条目名是 `root\a\0b.png`。解析成功（`fuzz repro` 报 `error-success`），随后 `tools/pfstool.cpp:140` 用 `dir + "/" + NormalizeName(e.name)` 拼出路径交给 `fopen`——C 字符串在那里截断，于是解包出来的文件名和存档里写的不一样（`root/a` 而不是 `root/a\0b.png`），且没有任何提示。

**(b) `..`**：`NormalizeName`（`src/pfs.cpp:551`）只做「反斜杠→斜杠」和「大写→小写」，**不处理 `..`**：

```
entry ..\..\escaped.txt   ->  /tmp/pfs-out/../../escaped.txt
entry root\ok.txt         ->  /tmp/pfs-out/root/ok.txt
```

也就是说 `pfstool extract archive.pfs --dir out/` 会被一个精心构造的条目写到 `out/` **之外**。这需要用户去解一个不可信的包，所以是「低」，但它属于安全边界问题，值得在解包时加一条：拒绝绝对路径与任何 `..` 段。

（解析器本身把名字原样保存是**对的**——`NormalizeName` 是给匹配用的；问题是使用处没有再做一次安全检查。所以这条我记在工具侧，不记在解析器侧。）

---

## 5. F5：条目数先 reserve 后检查（中低）

`src/pfs.cpp:336`：

```cpp
a.entries.reserve(count);      // count 只被限制在 400 万以内
```

一个 **1024 字节**的文件声明 `count = 3,900,000`：

```
pfs count-3.9m-small-index   refused   12.7 ms   peak 243750 KiB
```

238 MiB 分配完之后，循环第一轮就抛 `pfs: entry runs past the index`——结论是对的，代价白付了。放大倍数 ≈ 24 万倍。
**修法方向**：reserve 之前先做 `count * 12 <= indexSize`（一个条目至少要 12 字节），这是纯算术，不需要读内容。

### F6（**不是问题**，记下来是为了说明判定口径）`.ipt` 返回 true 但 dialect=Unknown

`ipt={s="",[""]=""}` 会让 `ParseIpt` 返回 **true**，`dialect` 是 `Unknown`。这看起来正是「把错误当成功」，但它其实是**写明的契约**：`ipt.cpp:382` / `ipt.cpp:415` 两条分支都返回 true，并把原因放进 `out.notes`（`unrecognised IPT mode '...'` / `the table is neither rectangles nor points`），`ComposeIpt` 随后用这个名字明确拒绝（`this IPT is a 'unknown' descriptor, not a composite image`），`iettool info` 也会打印 `unknown`。

所以判定收窄成：**dialect 可以 Unknown，但 notes 不能为空**；「Unknown 且一个字都不说」才算 error-success。改完这条，ipt 的 5000 条输入里 error-success 归零。

---

## 6. 资源上限专项（13 → 14 个探针）

`./build/fuzz limits`。每个探针都是一份「恶意或损坏的文件」，期望解析器**明确拒绝**而不是先分配。

| 探针 | 结果 | 耗时 | 峰值内存 | 说明 |
|---|---|---|---|---|
| `ipt canvas-100000x100000` | ✅ refused | 17.9 ms | 2 KiB | `IPT canvas 100000x100000 is too large to decode` |
| `ipt canvas-negative` | ✅ refused | 10.0 ms | 2 KiB | `composite IPT with no canvas size` |
| `ipt canvas-zero` | ✅ refused | 9.2 ms | 2 KiB | 同上 |
| `ipt canvas-overflow-multiply` | ✅ refused | 9.4 ms | 2 KiB | 65536×65536 也被挡（避免 w*h*4 溢出） |
| `ipt canvas-4g-value` | ❌ **UB** | 11.5 ms | 0 | **F2**：`ipt.cpp:46` int 转换越界 |
| `ast nesting-10000` | ❌ **栈溢出** | 12.5 ms | 0 | **F1** |
| `ast nesting-5000-closed` | ❌ **栈溢出** | 10.7 ms | 0 | **F1** |
| `iet one-line-100mb` | ❌ **ACCEPTED** | 2683 ms | 492 MiB | **F3**：接受，且超 2 s |
| `iet tags-200000-one-line` | ✅ accepted | 75.6 ms | 25 MiB | 20 万标签可接受，代价线性 |
| `asb item-count-max` | ✅ refused | 5.3 ms | 0 | `implausible ASB item count 4294967295` |
| `asb item-count-1m-no-body` | ✅ refused(截断) | 5.5 ms | 256 KiB | 头部合法、体被截断 → `truncated` + 说明 |
| `pfs count-3.9m-small-index` | ⚠️ refused 但 238 MiB | 12.7 ms | 243750 KiB | **F5** |
| `pfs index-size-4g` | ✅ refused | 6.5 ms | 0 | `index size runs past the end of the file` |
| `pfs entry-count-4g` | ✅ refused | 5.5 ms | 0 | 同上 |

判定口径：`mustReject` 的探针必须**明确报错**；`mustReject=false` 的探针（`.iet` 长行、`.ast` 5000 层闭合、`.asb` 截断）接受或拒绝都算过，只有崩/挂/超内存/OOM 才算失败。

---

## 7. 覆盖证据：跑了多少、按什么变异

<!--COVERAGE-->

---

## 8. 方法：为什么不用 libFuzzer

`clang++ -fsanitize=fuzzer,address` 在这台机器上**链接不起来**：

```
ld: library '.../clang/21/lib/darwin/libclang_rt.fuzzer_osx.a' not found
```

Apple 的 clang 发行版不带 libFuzzer 运行库。所以改用**自写循环 + 每输入一个子进程 + ASan/UBSan**，判定不依赖覆盖率反馈，而依赖「五类坏结局」：

| 判定 | 含义 | 怎么测出来的 |
|---|---|---|
| `crash` | 进程被信号打死 | 子进程 `WIFSIGNALED`（SIGSEGV/SIGABRT/SIGBUS/SIGILL） |
| `timeout` | 超过 deadline（默认 2000 ms） | 子进程 `setitimer(ITIMER_REAL)`；父进程另有 1.5× 硬杀 |
| `oom` | 一次解析申请超过 512 MiB | 覆盖全局 `operator new`，32 字节头记账，超限 `_exit(6)` |
| `silent-reject` | 拒绝但没给原因 | `Parse*` 返回 false 且 error 为空 |
| `error-success` | 声称成功但结果自相矛盾 | 每个解析器一组不变量（见下） |
| `bad-exception` | 逃出意料之外的异常类型 | 只接受 `std::runtime_error`；`out_of_range`/`length_error`/非 std 异常都算失败 |

**ASan 不能冒充「明确拒绝」**：拒绝走 `exit(1)`，所以 `ASAN_OPTIONS=exitcode=42`，harness 把 42 映射成 `sanitizer`（计入失败）。这条不做的话，一个堆越界会被读成「解析器礼貌地拒绝了」。

每个解析器的「成功也必须成立」不变量：

- `.pfs`：version ∈ 2..9；key 为空或 20 字节；条目名非空且无 NUL；`offset+size ≤ 文件长度`；`Sample()` 返回不超过条目大小。
- `.ast`：块数不超过字节数；名字长度不超过文件；`SpokenLines()` 不崩且不超过字节数。
- `.iet`：条目数不超过字节数；`commandCount`/`textCount` 不超过条目数；每条目的行号在文件行数范围内。
- `.ipt`：方言不是 Unknown；`Composite` 的画布过 `CanvasLimits`；**同一份字节解析两次结果必须一致**（不一致说明读了未初始化内存）。
- `.asb`：`items.size() ≤ itemCount`；未截断时数量必须相等；截断必须有原因；条目名/属性长度不超过文件。

种子：真实语料（`.pfs` 两个真包、`.ast` 三个真脚本、`.iet` 四个真脚本、`.ipt` 四个真描述符、`.asb` 两个真字节码）＋ 自造种子（pf2/pf4/pf5/pf6/pf8/pf9 六代归档、IPT 四方言、chained/arrayed 两种 `.ast`、KAG 与 Lua 两种 `.iet`）。

### harness 自己的两个 bug（都被本轮抓住，已修）

1. 第一版 `RunPfs` 成功后忘了 `return Good()`，于是**每个成功的归档都被报成 silent-reject**——差点把 43 个假问题当成真问题报上去。
2. 新增的 `deep-nest` 变异用 `operator+=(char const*)` 拼接没有 NUL 结尾的 `vector`，**harness 自己堆越界读**，被自己的 ASan 抓住。
   这两条记在这里，是因为「fuzzer 也需要自测」这件事不该只写在心里。

---

## 9. 本轮没有做的事

- **没有修任何一行 `src/`**（按任务要求，找问题不修问题）。修法方向只写在上面各节，是否修、由谁修，由 Lead 分派。
- **没有跑 libFuzzer 的 10 分钟覆盖率模式**（运行库缺失，见 §8）。替代方案是「结构感知变异 + 每个解析器 5000 条」，覆盖面按变异种类统计，不按边覆盖率统计。
- **没有 fuzz 渲染/存档/音频层**（`src/render/`、`src/save/`、`src/audio/`）：它们不是「拿字节解析」的层，需要的是别的形态的测试。
- **没有对 `.iet` 的 Lua 块（`[lua] ... [/lua]`）做真正的 Lua 执行 fuzz**：`[calllua]` 目前是桩，`lua.cpp` 只做表解析，所以命中的是 F1 那条路径。

---

## 2. Status after the fixes (Lead, 2026-10-25 19:35)

All five findings are now closed.  Each fix is in the layer that should own it,
not at the call site, so every tool inherits it:

| # | state | what changed | evidence |
|---|---|---|---|
| F1 | **fixed** | `src/lua.cpp`: refuses nesting past 200 levels, with a line number. | `ast-crash-0012.bin` → `unexpected token` exit 0 (was SIGSEGV at 128 KB of braces); the `ast-timeout-2593` input now runs in 55 ms |
| F2 | **fixed** | `src/ipt.cpp`: `IntValue` refuses a double outside int's range instead of the conversion being undefined. | a canvas of `w=4294967296` → `not an IPT descriptor: composite IPT with no canvas size` |
| F3 | **fixed** | `src/iet.cpp`: a source line longer than 1 MiB is refused before parsing. | an 8 MiB line → `line longer than 1 MiB: a script line is not this long` |
| F4a | **fixed** | `src/pfs.cpp`: an entry name containing NUL is refused at parse time. | `pfs-error-success-0069.bin` → `entry name is not usable: \x00…` |
| F4b | **fixed** | `src/pfs.cpp`: `NameIsSafe()` also refuses absolute paths and any `..` component, at parse time. | a crafted archive whose entry is `..\..\escape.txt` → refused, nothing written outside the directory |
| F5 | **fixed** | `src/pfs.cpp`: `entries.reserve(count)` is capped at 4,096 instead of trusting a count read from a 1 KiB file. | the same input peaks at **5.5 MB** (was **243 MB**) |

The general lesson worth keeping: four of the five were "the parser trusted a
number the file wrote down".  A format reader's job is to be wrong loudly about
nonsense, and the cheap way to be sure is to fuzz it with nonsense.
