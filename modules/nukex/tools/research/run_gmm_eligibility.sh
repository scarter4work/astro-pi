#!/usr/bin/env bash
# Isolates ONE variable that Stage 1's corpus table confounded: whether the
# frame count clears ModelSelector::Config::min_samples_for_gmm (30).
#
# Same corpus, same code, same normalisation setting -- only the frame count
# changes, from 30 (GMM eligible) to 12 (GMM never runs).  Stage 1 compared
# M16-at-12-frames against NGC7635-at-65 and read the difference as a property
# of the corpora.
set -uo pipefail
R=$(cd "$(dirname "$0")" && pwd)/run_mu_corpus.sh
L=$HOME/.cache
"$R" m16_12f /home/scarter4work/projects/processing/M16 12 norm   >> "$L/nukex_huber_m16_12f_norm.log"   2>&1
"$R" m16_12f /home/scarter4work/projects/processing/M16 12 nonorm >> "$L/nukex_huber_m16_12f_nonorm.log" 2>&1
echo "GMM ELIGIBILITY RUNS COMPLETE"
