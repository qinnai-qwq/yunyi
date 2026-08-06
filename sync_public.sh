#!/usr/bin/env bash
# ============================================================
# sync_public.sh — 把 master(全量) 的开源部分同步到 public 分支并推送 qinnai-qwq
#
# 用法:  bash sync_public.sh
#
# 前提:
#   - 已在本地仓库目录（master 工作区干净）
#   - gh CLI 已登录 qinnai-qwq 账号（gh auth switch --user qinnai-qwq）
#
# 安全设计:
#   ★ 绝不 merge master —— 那会把 app/ 等私有源码带进 public 分支的历史。
#     改用「只复制公开路径」：git checkout master -- <公开路径> 逐个覆盖，
#     public 分支的历史永远只包含公开内容。
# ============================================================
set -e

GH_ACCOUNT="qinnai-qwq"
TOKEN=""
if command -v gh >/dev/null 2>&1; then
  TOKEN="$(gh auth token -u "$GH_ACCOUNT" 2>/dev/null || true)"
fi
if [ -n "$TOKEN" ]; then
  PUSH_URL="https://${GH_ACCOUNT}:${TOKEN}@github.com/qinnai-qwq/yunyi.git"
else
  echo "[warn] 未获取到 qinnai-qwq 的 gh token，使用 release remote 默认凭据"
  PUSH_URL="release"
fi

# 公开路径白名单：只同步这些（master 的 app/dist/third_party/webview2-sdk 永不进入）
PUBLIC_PATHS=(
  ".gitattributes"
  ".gitignore"
  "NetEngine"
  "README.md"
  "SVG"
  "docs"
  "icon.ico"
  "resource.rc"
  "云驿.slnx"
  "云驿.vcxproj"
  "云驿.vcxproj.filters"
  "云驿GUI.vcxproj"
)

echo "[1/4] 切到 public 分支..."
git checkout public

echo "[2/4] 从 master 复制公开路径（选择性，不 merge）..."
for p in "${PUBLIC_PATHS[@]}"; do
  git checkout master -- "$p" 2>/dev/null || echo "  (跳过不存在: $p)"
done

echo "[3/4] 提交并推送 qinnai-qwq..."
git add -A
git commit -m "sync: 同步 master 开源部分到 public" 2>/dev/null || echo "  无变更，跳过提交"
git push --force-with-lease "$PUSH_URL" public:master

echo "[4/4] 切回 master..."
git checkout master

echo "✅ 同步完成"
