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

## 下一步

1. 弄清 `.ast` 里各个键的**语义**（哪些是场景名、哪些是台词、tag 如何驱动文本）
   —— 这一步需要真实游戏脚本来对照
2. `.iet` 图像格式
2. `.iet` 图像格式
3. 运行时骨架：脚本 VM + 画面层
4. 需要真实 Artemis 游戏数据才能验收
