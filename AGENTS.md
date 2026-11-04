# SPEC-Driven Development 约束文档
- 参考 Notion Page ID: 37d0ec7a4b9280bd8c84c922f57f6d3e

# 全局研发环境与行为准则
- 参考 Notion Page ID: 37d0ec7a4b928005aee8d863af5b2518

# 当前项目需求文档
- 参考 Notion Page ID: 40e0ec7a4b9280a6b9d4f8df17ebf4d0
- 变更记录 Notion Page ID: 40e0ec7a4b9280a2ae7dd94796045a7b

# 当前项目设计文档
- 参考 Notion Page ID: 40e0ec7a4b92802d95a4f625893fbd78
- 变更记录 Notion Page ID: 40e0ec7a4b9280b4870bf1500b7cb0d2

# graphify

This project has a graphify knowledge graph at graphify-out/.

Rules:
- Before answering architecture or codebase questions, read graphify-out/GRAPH_REPORT.md for god nodes and community structure
- If graphify-out/wiki/index.md exists, navigate it instead of reading raw files
- For cross-module "how does X relate to Y" questions, prefer `graphify query "<question>"`, `graphify path "<A>" "<B>"`, or `graphify explain "<concept>"` over grep — these traverse the graph's EXTRACTED + INFERRED edges instead of scanning files
- After modifying code files in this session, run `graphify update .` to keep the graph current (AST-only, no API cost)
