<div align="center">
  <h1>ewebview</h1>
  <p>面向资源受限系统和裸机环境的自包含嵌入式 Web 引擎。</p>
  <p><a href="README.md">English</a> | <a href="README.zh-CN.md">简体中文</a></p>
  <p><a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" alt="许可证：Apache-2.0"></a></p>
  <table>
    <tr><th>平台</th><th>Runner</th><th>CI 状态</th></tr>
    <tr><td>macOS</td><td><code>macos-14</code></td><td><a href="https://github.com/MisaZhu/ewebview/actions/workflows/ci.yml"><img src="https://github.com/MisaZhu/ewebview/actions/workflows/ci.yml/badge.svg?branch=main&job=build%20%28macos-14%29" alt="macOS CI"></a></td></tr>
    <tr><td>Linux</td><td><code>ubuntu-latest</code></td><td><a href="https://github.com/MisaZhu/ewebview/actions/workflows/ci.yml"><img src="https://github.com/MisaZhu/ewebview/actions/workflows/ci.yml/badge.svg?branch=main&job=build%20%28ubuntu-latest%29" alt="Linux CI"></a></td></tr>
  </table>
</div>

**ewebview** 是一个自包含的嵌入式 Web 引擎，支持 HTML、CSS 和 JavaScript，面向资源受限操作系统及裸机环境。它将页面渲染到抽象的 ARGB8888 内存画布（`eweb_surface_t`），图形、字体、图片解码、网络和时钟等平台能力通过 `eweb_port_t` 回调表注入，核心代码不依赖特定操作系统或窗口系统。

## 特性

- **HTML / CSS**：基于 litehtml 和 Gumbo，支持常用 CSS2.1/3、媒体查询、表格和 Flex 布局。
- **JavaScript**：集成 mario VM，提供 DOM、Event、BOM 和 Canvas 2D 桥接层，支持内联及外部脚本。
- **Canvas 2D**：支持路径、渐变、图案、虚线、阴影、ImageData、drawImage 和 Path2D。
- **网络**：内置 BearSSL 的 HTTP/HTTPS 客户端，并支持 `file://` 和 `res://` 协议。
- **多线程管线**：引擎线程与下载线程解耦，UI 线程无需阻塞等待网络或渲染。
- **可移植**：平台能力统一通过 HAL 回调表接入，支持 SDL2 和 EwokOS 参考移植。
- **HiDPI**：SDL2 移植支持设备像素比设置，布局使用 CSS 像素，光栅化使用原生分辨率。

## 构建

默认构建使用 SDL2 桌面移植，支持 macOS 和 Linux。请先安装 SDL2、SDL2_ttf、SDL2_image、SDL2_gfx 及 `pkg-config`，然后执行：

```sh
git submodule update --init --recursive
make
```

构建产物位于 `build_aarch64/virt/`，包括 `lib/libewebview.a` 和 `bin/sdlbrowser`。清理构建产物：

```sh
make clean
```

EwokOS 交叉构建：

```sh
make PORTING=ewokos OS_TYPE=ewokos
```

## 运行 SDL2 示例

```sh
./build_aarch64/virt/bin/sdlbrowser
./build_aarch64/virt/bin/sdlbrowser https://www.w3.org
```

## 目录结构

- `ewebview/`：引擎核心及平台移植层
- `litehtml/`：HTML/CSS 布局引擎
- `jsnative/`、`mario_js/`：JavaScript VM 及浏览器桥接层
- `libtinyhttpsc/`：HTTP/HTTPS 客户端
- `libwebp/`、`libplutovg/`：图片及 SVG 光栅化支持
- `bin/sdlbrowser/`：SDL2 桌面示例程序
- `docs/wiki/`：架构、线程模型、页面管线、Canvas、网络和嵌入指南

## 文档

- [英文架构文档](docs/wiki/README.md)
- [中文架构文档](docs/wiki/README.zh.md)

## 许可证

项目主体使用 Apache License 2.0。litehtml/gumbo、mario_js、libwebp 和 libtinyhttpsc 分别保留其上游许可证。
