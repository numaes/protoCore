# First session (2026-10-19, superseded)

Kept for the record; not used by the report's tables.  Every after-side
(2.14.7) run loaded protoCore from one file instance of the staged library
whose text placement made protoPython 7-12 % slower with identical
instructions (fresh copies of the same bytes ran normally), and the two
sides ran separately built runtime binaries.  The second session
(`../results/`, `../rerun2.sh`) runs both sides on the same runtime
binaries with a fresh library copy per run.
