#!/usr/bin/env python3
import subprocess, random, os, sys, shutil

CODE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./code")
WORKDIR = "/tmp/stress_run"

def run_chunks(chunks, fresh=True):
    """Run the program over each chunk (separate invocations), return combined output."""
    if os.path.exists(WORKDIR):
        shutil.rmtree(WORKDIR)
    os.makedirs(WORKDIR)
    shutil.copy(CODE, WORKDIR)
    outputs = []
    for chunk in chunks:
        inp = f"{len(chunk)}\n" + "\n".join(chunk) + "\n"
        p = subprocess.run([os.path.join(WORKDIR, "code")], input=inp.encode(),
                           capture_output=True, cwd=WORKDIR, timeout=120)
        if p.returncode != 0:
            print("RUNTIME ERROR, rc =", p.returncode)
            print("stderr:", p.stderr.decode()[:2000])
            sys.exit(1)
        outputs.append(p.stdout.decode())
    return "".join(outputs)

def reference(ops):
    data = {}
    out = []
    for op in ops:
        parts = op.split()
        if parts[0] == "insert":
            k, v = parts[1], int(parts[2])
            data.setdefault(k, set()).add(v)
        elif parts[0] == "delete":
            k, v = parts[1], int(parts[2])
            if k in data and v in data[k]:
                data[k].remove(v)
                if not data[k]:
                    del data[k]
        else:
            k = parts[1]
            if k in data and data[k]:
                out.append(" ".join(str(v) for v in sorted(data[k])))
            else:
                out.append("null")
    return "".join(s + "\n" for s in out)

def gen_ops(n, nkeys, valrange, seed, delfrac=0.3, findfrac=0.3):
    rng = random.Random(seed)
    keys = [f"key{rng.randrange(nkeys)}" for _ in range(nkeys)] if nkeys <= 10000 else None
    ops = []
    inserted = []
    for i in range(n):
        r = rng.random()
        if r < 1 - delfrac - findfrac:
            if keys is not None:
                k = rng.choice(keys)
            else:
                k = "k" + str(rng.randrange(nkeys))
            v = rng.randrange(-valrange, valrange)
            ops.append(f"insert {k} {v}")
            inserted.append((k, v))
        elif r < 1 - findfrac:
            if inserted and rng.random() < 0.7:
                k, v = rng.choice(inserted)
            else:
                k = "k" + str(rng.randrange(nkeys))
                v = rng.randrange(-valrange, valrange)
            ops.append(f"delete {k} {v}")
        else:
            if keys is not None:
                k = rng.choice(keys)
            elif inserted and rng.random() < 0.5:
                k = rng.choice(inserted)[0]
            else:
                k = "k" + str(rng.randrange(nkeys))
            ops.append(f"find {k}")
    return ops

def chunk_ops(ops, rng, nchunks):
    if nchunks <= 1:
        return [ops]
    cuts = sorted(rng.sample(range(1, len(ops)), nchunks - 1))
    chunks, prev = [], 0
    for c in cuts:
        chunks.append(ops[prev:c]); prev = c
    chunks.append(ops[prev:])
    return chunks

def main():
    tests = [
        # (name, n, nkeys, valrange, seed, delfrac, findfrac, nchunks)
        ("small_dense", 2000, 20, 100, 1, 0.35, 0.35, 3),
        ("small_sparse", 2000, 500, 10**9, 2, 0.3, 0.3, 2),
        ("medium_dense", 20000, 50, 1000, 3, 0.4, 0.3, 4),
        ("medium_med", 30000, 3000, 10**6, 4, 0.3, 0.3, 3),
        ("big_vals", 10000, 200, 2**31 - 1, 5, 0.3, 0.3, 2),
        ("churn", 20000, 100, 50, 6, 0.45, 0.25, 5),
        ("one_key", 5000, 1, 10**6, 7, 0.4, 0.3, 3),
        ("delete_heavy", 15000, 300, 500, 8, 0.6, 0.2, 3),
    ]
    for name, n, nkeys, valrange, seed, delfrac, findfrac, nchunks in tests:
        ops = gen_ops(n, nkeys, valrange, seed, delfrac, findfrac)
        chunks = chunk_ops(ops, random.Random(seed + 1000), nchunks)
        got = run_chunks(chunks)
        exp = reference(ops)
        if got == exp:
            print(f"PASS {name} (n={n}, keys={nkeys}, chunks={nchunks})")
        else:
            g = got.splitlines(); e = exp.splitlines()
            print(f"FAIL {name}: got {len(g)} lines, expected {len(e)} lines")
            for i, (a, b) in enumerate(zip(g, e)):
                if a != b:
                    print(f"  first diff at find #{i}:\n    got:      {a[:120]}\n    expected: {b[:120]}")
                    break
            sys.exit(1)
    print("ALL STRESS TESTS PASSED")

if __name__ == "__main__":
    main()
