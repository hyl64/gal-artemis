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

## 下一步

1. `.ast` 脚本字节码：头、指令集、场景结构（`docs/AST.md`）
2. `.iet` 图像格式
3. 运行时骨架：脚本 VM + 画面层
4. 需要真实 Artemis 游戏数据才能验收
