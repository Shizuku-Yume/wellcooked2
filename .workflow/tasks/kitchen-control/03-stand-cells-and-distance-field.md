# 03: 站位格表与距离场

**Effort:** kitchen-control
**Task type:** feature
**Base commit:** <commit-sha>
**Base branch:** small/os-core-acceptance-fixes

**Delivers:** 加载 `kitchen.json` 时为每个工位算出带朝向的站位格（≤ 4 个、按距离排序）和
到全图的 BFS 距离场；没有站位格的工位让加载失败并报出工位 id。

**Blocked by:** 01
**Files:** `bridge/include/kitchenos/stand.h`、`bridge/src/stand.c`、`bridge/src/kitchen.c`、
`bridge/tests/test_stand.c`、`bridge/Makefile`

**Read first:** `.workflow/specs/kitchen-control.md` 的"L1 走位 / 站位格表与距离场"与 W1；glossary
站位格、距离场；`bridge/include/kitchenos/{kitchen,nav}.h`
**Covers:** W1

**Check:** `cd bridge && make test` → `test_stand` 对 `config/maps/` 四张地图全过：每个工位
≥ 1 个站位格、站位格可走、朝向边未被 `mask` 挡；距离场在站位格上为 0、相邻格差 ≤ 1；
删掉某工位全部邻格后加载失败且错误里有该工位 id。

- [ ] 站位格带朝向（+x/-x/+z/-z），两个工位共用一格时两边都列出
- [ ] 距离场 `uint16`，不可达记最大值；内存按 64 工位 × 2048 格上限静态分配
- [ ] `nav` 的"工位相邻可走格"改用站位格表，旧逻辑删掉；距离场接口给 `navd` 与 Scheduler 共用
