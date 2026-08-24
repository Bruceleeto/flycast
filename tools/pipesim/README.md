# pipesim tooling

Builds and checks `core/hw/sh4/cachesim/pipesim.{h,cpp}`, the SH4 pipeline
model. See `docs/cachesim/pipesim_notes.md` for what is known wrong.

| file | what it does |
|---|---|
| `gen_optable_rw.py` | generates `pipesim_optable.h` from flycast's own opcode table, deriving register read/write sets from the operand format |
| `isa_vocabulary.txt` | 45,661 instruction forms both the reference simulator and `sh-elf-as` accept unambiguously, one 16-bit instruction each |
| `make_hwsafe_vocab.py` | narrows that to forms safe to run on hardware and safe to address memory with; prints a reason per exclusion |
| `gen_corpus.py` | random blocks, sampled per mnemonic so rare instructions appear |
| `ref_runner.js` | runs a corpus through the reference simulator, one block per call |
| `predict.sh` | the model's prediction for the same corpus |
| `difftest.sh` | assemble once, run both, diff |
| `gen_hwtest.py` | emits a Dreamcast program that measures the corpus on real silicon |
| `compare_hw.py` | joins hardware measurements against model predictions |
| `pipetest.cpp` | standalone driver; builds to `build/pipetest` |

## Build the driver

```
g++ -std=c++17 -O2 -Icore -Icore/deps -Icore/deps/nowide/include -Icore/khronos \
    -o build/pipetest tools/pipesim/pipetest.cpp core/hw/sh4/cachesim/pipesim.cpp
```

## Check the model against the reference

```
python3 tools/pipesim/gen_corpus.py 200 42 --dense \
    --vocab tools/pipesim/isa_vocabulary_hwsafe_sp.txt > corpus.asm
node --stack-size=8000 tools/pipesim/ref_runner.js corpus.asm > ref.tsv
tools/pipesim/predict.sh corpus.asm > model.tsv
```

Blocks where `ref.tsv` says `FAIL`, or where its cycle count is 1000 or more,
are the reference's own limits and must be excluded from scoring.

## Check it against hardware

See `build/pipehw/README.md`.
