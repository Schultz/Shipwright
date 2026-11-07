import json, re, sys
from collections import defaultdict, Counter

S = sys.argv[1]
GID = sys.argv[2] if len(sys.argv) > 2 else "10113"
game = json.load(open(f"{S}/p{GID}.json"))["PatchData"]
game["Achievements"] = {a["ID"]: a for a in game["Achievements"]}
for lb in game.get("Leaderboards") or []:
    game["Achievements"]["LB%d" % lb["ID"]] = {"ID": 0, "Title": "[LB] " + lb["Title"], "Description": lb["Description"], "MemAddr": lb["Mem"].replace("::", "_")}
notes = {int(c["Address"], 16): c["Note"].split("\n")[0][:60]
         for c in json.load(open(f"{S}/notes10113.json"))["CodeNotes"]}

# NTSC-U 1.0 RAM layout (offsets from 0x80000000), from the zeldaret decomp
REGIONS = [
    (0x11A5D0, 0x11A5D0 + 0x1428, "SaveContext"),
    (0x11D730, 0x11D740, "Input (padmgr)"),
    (0x1C6EA0, 0x1C84A0, "gGameInfo / REG"),
    (0x1C84A0, 0x1DA9B8, "PlayState"),
    (0x1DAA30, 0x1DB4C4, "Player actor"),
    (0x1DB4C4, 0x400000, "Actor heap (dynamic)"),
]
def region(a):
    for lo, hi, n in REGIONS:
        if lo <= a < hi:
            return n
    return "Other"

SIZES = {"": "16", "0xH": "8", "0x ": "16", "0xW": "24", "0xX": "32", "0xL": "lo4", "0xU": "hi4",
         "0xM": "b0", "0xN": "b1", "0xO": "b2", "0xP": "b3", "0xQ": "b4", "0xR": "b5", "0xS": "b6",
         "0xT": "b7", "0xK": "bitcount", "fF": "float", "fB": "floatBE", "0xI": "16BE", "0xJ": "24BE", "0xG": "32BE"}
OPER = re.compile(r"([dpbv~]?)(0x[HWXLUMNOPQRSTKIJG ]?|f[FBHIM])([0-9a-fA-F]+)")

per_ach = []
reads = Counter()
read_regions = defaultdict(set)
ptr_reads = Counter()
for a in game["Achievements"].values():
    mem = a["MemAddr"]
    regs = set(); uses_ptr = False
    for group in re.split(r"(?<![0-9a-fA-Fx])S", mem):
        prev_addaddr = False
        for cond in group.split("_"):
            flag = cond[0] if len(cond) > 1 and cond[1] == ":" else ""
            body = cond[2:] if flag else cond
            for m in OPER.finditer(body):
                pre, sz, hexa = m.groups()
                if sz == "0x" and not hexa:  # constant
                    continue
                addr = int(hexa, 16)
                if prev_addaddr:
                    uses_ptr = True
                    ptr_reads[(sz, addr)] += 1
                    regs.add("Pointer-relative")
                    continue
                if addr < 0x1000 and sz == "0x" :
                    pass
                r = region(addr)
                regs.add(r); reads[(addr, sz)] += 1; read_regions[r].add(addr)
            prev_addaddr = flag == "I"
    per_ach.append((a["ID"], a["Title"], a["Description"], sorted(regs), uses_ptr))

print(f"== {game['Title']}: {len(per_ach)} achievements, {len(game.get('Leaderboards') or [])} leaderboards ==\n")
cat = Counter()
for _, t, d, regs, p in per_ach:
    if regs == ["SaveContext"]: cat["SaveContext only"] += 1
    elif set(regs) <= {"SaveContext", "PlayState", "Input (padmgr)", "gGameInfo / REG"}: cat["Save + PlayState/input (fixed globals)"] += 1
    elif "Actor heap (dynamic)" in regs: cat["Touches dynamic actor heap"] += 1
    elif "Pointer-relative" in regs: cat["Uses pointer chains"] += 1
    elif "Player actor" in regs: cat["Player actor (fixed address)"] += 1
    else: cat["Other: " + ",".join(regs)] += 1
print("Achievements by hardest region touched:")
for k, v in cat.most_common(): print(f"  {v:3d}  {k}")
print("\nDistinct addresses per region:")
for r, s in sorted(read_regions.items(), key=lambda x: -len(x[1])): print(f"  {len(s):3d}  {r}")
print(f"  {len(ptr_reads):3d}  pointer-relative offsets")

print("\nNon-SaveContext addresses:")
for (addr, sz), c in sorted(reads.items()):
    r = region(addr)
    if r == "SaveContext": continue
    print(f"  0x{addr:06x} {sz:4s} x{c:<3d} {r:22s} {notes.get(addr, '(no note)')}")
if ptr_reads:
    print("\nPointer-relative reads:")
    for (sz, off), c in sorted(ptr_reads.items(), key=lambda x: x[0][1]): print(f"  +0x{off:x} {sz} x{c}")

print("\nPer-achievement:")
for i, t, d, regs, p in per_ach:
    print(f"  {i:6d} {t[:34]:34s} | {', '.join(r for r in regs if r!='SaveContext') or 'save only'}")
