# Q4 -> Q6, every clean pair

Both halves of every pair ran under one configuration. Arms from the
contaminated 2026-08-03 ledger are excluded; see ../quant-ledger-CONTAMINATED/.

| corpus | n | family | Q4 | Q6 | delta |
| --- | ---: | --- | ---: | ---: | ---: |
| gold70 | 69 | E2B | 0.6714 | 0.6569 | -0.0145 |
| gold70 | 69 | E4B | 0.7083 | 0.7660 | +0.0577 |
| v5small | 1001 | E2B | 0.6138 | 0.6138 | +0.0000 |
| v5small | 1001 | E4B | 0.6202 | 0.6396 | +0.0194 |
| v3small | 1001 | E2B | 0.5530 | 0.5832 | +0.0302 |
| v3small | 1001 | E4B | 0.5770 | 0.5815 | +0.0045 |
| v8-baseline | 1001 | E2B | 0.6114 | 0.6179 | +0.0065 |
| v8-baseline | 1001 | E4B | 0.6189 | 0.6339 | +0.0150 |
| v5-large | 10000 | E4B | 0.6324 | 0.6450 | +0.0126 |

- **E2B**: 2/4 positive. Deltas: -0.0145, +0.0000, +0.0302, +0.0065
- **E4B**: 5/5 positive. Deltas: +0.0577, +0.0194, +0.0045, +0.0150, +0.0126

At n=10,000 the E4B step is +0.0126, 95% CI [+0.0072, +0.0181], and the
Q6->Q8 step is -0.0129, CI [-0.0179, -0.0078]. Both exclude zero, so Q6 is
a peak rather than a stopping point. No E2B comparison excludes zero.
