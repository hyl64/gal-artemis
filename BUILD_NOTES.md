# Engine ⑤ Artemis — 从零重写（进行中）

## 状态：🚧 第一层完成（.pfs 归档容器），AST 脚本未开始

Artemis 没有开源实现可移植，所以这一层是**从零写的**：没有上游源码，只有对格式
的记录（[pfs-rs](https://github.com/sakarie9/pfs-rs)、
[artemis_ast](https://github.com/xmoezzz/artemis_ast)、
[aluigi 的 QuickBMS 脚本](http://www.aluigi.zenhax.com/viewtopic.php@t=9234.html)
是了解格式的参考，代码是自己写的）。

## 已完成：`.pfs` 归档（PF6 / PF8）

```
artemis/
  src/pfs.h src/pfs.cpp   读写实现（167 行）
  tools/pfstool.cpp        list / extract / create / selftest
  docs/PFS.md              格式记录
```

**自证（不需要游戏数据）**：
```bash
$ ./build/pfstool selftest
built a 4230 byte archive
read back: pf8, 2 file(s)
  root/script/main.ast         20 byte(s)  identical
  root/image/bg01.iet        4096 byte(s)  identical
  content encrypted in the archive: yes
all passed

$ ./build/pfstool create /tmp/test.pfs root/a.txt=/tmp/a.txt root/b.bin=/tmp/b.bin
wrote /tmp/test.pfs: 2 file(s), 2124 byte(s)
$ ./build/pfstool list /tmp/test.pfs
          95          29  root/a.txt
         124        2000  root/b.bin
$ ./build/pfstool extract /tmp/test.pfs /tmp/pfsout
2 file(s) written to /tmp/pfsout      # cmp: 两个文件都逐字节一致
```

**格式要点**（详见 `docs/PFS.md`）：
- 头：`magic[3]` + `u32 index_size` + `u32 index_count`，条目从 0x0B 开始
- 内容 XOR 加密，密钥 = `SHA1(索引字节)` 的 20 字节，按**文件在归档内的偏移**推进
- `index_size` 从 0x07 起算，包含 count、条目和尾部表

**踩过的两个坑**：索引尾部少写 u32（声明长度与实际不符 → 偏移全错）；密钥哈希漏了索引
开头的 count（解密后内容对不上）。

## 已完成：`.ast` 剧本脚本（第二层）

**关键发现**：Artemis 的剧本是**文本**，不是字节码——`root/script/*.ast` 是一个
赋值树（`astver = "2.0"`、`text = { "start" = { ... } }`），文本里的 `[123]` 是
运行时动作标记（停顿/控制）。

```
artemis/
  src/ast.h src/ast.cpp    词法 + 递归下降解析 + 回写
  tools/asttool.cpp        dump / roundtrip / selftest
  docs/AST.md              语法记录
```

**自证（不需要游戏数据）**：
```bash
$ ./build/asttool selftest
script: 211 byte(s)
all passed          # 含 CRLF 版本和单行脚本
```

不变量：**parse → dump → parse 不丢信息**（两棵树的标准形式必须相等）。

```bash
$ ./build/asttool dump /tmp/sample.ast
astver = "2.0"
text = {
	start = {
		name = "start"
		lines = {
			"俺たちの新しい日常が、ここから始まる。"
			[12]
			"二行目"
		}
		volume = 128
	}
}
```

**踩过的坑**：块里的键可以是**带引号的字符串**（`"start" = {`），我第一版只认裸标识符
作键，直接报错；块是字典还是列表要**逐条判断**，因为同一个文件里两种写法混用。

## 已完成：剧本台词抽取（第三层能力）

`asttool text <script> [--sjis]`：按顺序抽出剧本里玩家会看到的台词（跳过 `name`/`astver`
这类簿记字段，保留 `[123]` 动作标记），并支持 **Shift-JIS → UTF-8** 转换
（老作品是 SJIS，新作品是 UTF-8）。

```bash
$ ./build/asttool text /tmp/sample.ast
/tmp/sample.ast: 3 line(s)
  | 俺たちの新しい日常が、ここから始まる。
  | [12]
  | 二行目

$ ./build/asttool text /tmp/sample_sjis.ast --sjis     # 同一个剧本的 SJIS 版本
/tmp/sample_sjis.ast: 3 line(s)
  | 俺たちの新しい日常が、ここから始まる。
  | [12]
  | 二行目
```

这就是「把剧本读成台词」的能力——不需要游戏数据也能验证（`selftest` 与 `--sjis` 都有自证）。

## 已完成：剧本执行核心（第四层，运行时的脑子）

`src/runtime.{h,cpp}` + `tools/arun.cpp`：把 `.ast` 走成**动作流**——这是引擎里与平台
无关的部分，也是从零重写最该先写的「脑子」。

支持的动作：
| 动作 | 来源 |
|---|---|
| `line` | 剧本里的字符串（玩家读到的一行） |
| `wait` | `[800]` 这样的标记（停顿毫秒） |
| `jump` | `goto = "scene"`（跨场景跳转） |
| `unknown` | 认不出的键，**如实报告而不静默忽略** |

```bash
$ ./build/arun /tmp/scene.ast
scenes: start next
line    はじまりの朝。   (scene start)
wait    800 ms   (scene start)
line    窓の外は、まだ青い。   (scene start)
jump    next   (scene next)
line    そして物語は続く。   (scene next)
wait    120 ms   (scene next)
line    終わり。   (scene next)
4 line(s), 2 pause(s) totalling 920 ms, 1 jump(s), 0 unknown

$ ./build/arun --selftest
lines=4 pauses=2 waited=920 jumps=1 unknown=0 -> all passed
```

**自证**：内置一个「两场景 + 停顿 + 跳转」的剧本，断言 4 行 / 2 停顿 / 共 920ms / 1 跳转，
不需要游戏数据。

**踩到的坑**：解析出的剧本是构造函数的局部对象，而运行时把 `&m.second` 存了下来——
**悬垂指针**，第二个场景读到垃圾。改成按值持有节点后正常。

## 已完成：第一屏（第五层，能玩了）

`tools/agame.cpp`：SDL2 窗口 + CoreText 渲日文，把运行时动作流画出来——夜色渐变背景、
对话框、当前台词；空格/点击推进，`--frames <dir>` 无头模式每行写一张图。

```bash
$ SDL_VIDEODRIVER=dummy ./build/agame tests/sample-scene.ast --frames /tmp/frames
frame 0: はじまりの朝。
frame 1: 窓の外は、まだ青い。
frame 2: そして物語は続く。
frame 3: 終わり。
agame: 4 frame(s)
```
产物：`screenshots/first-screen.png`（1280×720，日文台词正确）。

**踩到的坑（CoreText 的「一切来自属性字典」）**：

`CTLineDraw` **完全不看上下文状态**，颜色、字体、字号全在属性字典里：
1. 不设 `kCTForegroundColorAttributeName` → **黑字**（深色框上等于看不见）。
   第一版量到「458 个不透明像素」却「0 个亮像素」。
2. 不设 `kCTFontAttributeName` → 用**默认 12pt**，不是 `CTFontCreateWithName` 建的那个。
   表现是「字体名和字号都报 42pt，但行宽只有 92px」——7 个汉字应该是 294px。
   加上字体属性后：`chars=7 width=294.0 ascent=37.0` ✓

3. **CGBitmapContext 的字节序要显式**：用 `kCGImageAlphaPremultipliedLast` 并按字节
   RGBA 读，不猜 `uint32` 的顺序（猜错一次，alpha 和 R 通道互换）。

这三条的共同点：**CoreText 是声明式的，不是状态式的**——你在上下文里设的任何东西
都可能被忽略。

## 下一步

1. 弄清 `.ast` 里各个键的**语义**（哪些是场景名、哪些是台词、tag 如何驱动文本）
   —— 这一步需要真实游戏脚本来对照
2. `.iet` 图像格式
2. `.iet` 图像格式
3. 运行时骨架：脚本 VM + 画面层
4. 需要真实 Artemis 游戏数据才能验收

---

## 引擎⑤ 总览（第 50 轮）

| 层 | 文件 | 工具 | 自证 |
|---|---|---|---|
| `.pfs` 归档（PF6/PF8 + XOR 加密） | `src/pfs.*` | `pfstool` | `selftest` |
| `.ast` 文本剧本（解析 + 回写） | `src/ast.*` | `asttool` | `selftest` |
| 台词抽取 + Shift-JIS | — | `asttool text --sjis` | 两种编码同结果 |
| 剧本执行核心（动作流） | `src/runtime.*` | `arun` | `--selftest` |
| 第一屏（SDL + CoreText） | `tools/agame.cpp` | `agame` | `--frames` 逐行出图 |

**1829 行自写代码 / 4 个工具 / 5 套自证 / 2 篇格式文档 / 1 张截图。**

## 已完成：打字机效果（第六层能力）

台词**逐字显示**，无头模式把过程分阶段写成帧，便于检验：

```
frame 0: は
frame 1: はじま
frame 2: はじまりの朝。
```

产物：`screenshots/typewriter.png`（三个阶段叠在一起）。

**坑**：第一版按**字节**切片，把 UTF-8 汉字切成半个 → 乱码。
改成按字符边界切（`FirstChars` 会跳过续字节）。

## 已完成：真实游戏的文件布局 + 分卷归档（第七层能力）

**没有 Artemis 游戏数据**，但公开的逆向分析文章记录了真实发行版的布局
（[GalgeLabo 的 [RE] 文章](https://galgelabo.fc2.page/amakano-3-re)，引擎标注为 Artemis）：

```
<Game>.exe
<Game>.pfs.000   角色语音
<Game>.pfs.001   角色立绘
<Game>.pfs.002   背景、事件CG、立绘
<Game>.pfs.003   事件CG
```

**关键**：`.pfs.NNN` 每个都是**独立的归档**（不是分片），`pfsgame` 现在能按后缀顺序
遍历整个游戏目录：

```bash
$ ./build/pfsgame /tmp/game
Demo: 2 archive(s)
  Demo.pfs.001
  Demo.pfs.002
2 entries
          29  Demo.pfs.001:root/a.txt
        2000  Demo.pfs.002:root/b.bin
```

**另一个重要发现**：游戏内部的资源路径是

```
root\image\ev\ev_<尺寸><组>_<Zoning>_<ID>_<差分>.<ext>
root\image\fg\<角色>_<ID>.psb      ← 立绘是 Emote 的 PSB 格式，不是位图！
```

`.psb`（Emote 的 tag 式场景格式）和 `.iet`（静图）**都不是能直接解码的位图**。
`.iet` 没有公开文档，只能靠真实文件的字节反推。

**还缺**：`.iet` 解码、`.psb` 立绘、存档、音频。
