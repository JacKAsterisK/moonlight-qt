# Clipboard HTTPS integration tests

Run `tests/clipboard/run_integration.ps1` from a Windows development checkout
with its existing Qt/MSVC tools. Optional parameters override Qt, OpenSSL and
the Visual Studio environment script paths.

The fixture links the actual `ClipboardSync` implementation. It listens only
on loopback, uses newly generated server/client certificates, and supplies
in-memory clipboard read/write callbacks on the owning thread. It initializes
only SDL events and does not access the Windows clipboard or a saved Moonlight
profile. Generated keys and build files remain inside ignored `.tools` test
directories; no keys or clipboard contents are printed.

Tests cover HTTPS client authentication, bidirectional changes without echo,
stale responses versus newer local copies, delivery-time re-sampling, non-text
clipboard changes, unsupported hosts, mismatched paired certificates, redirects,
oversized/invalid text, retrying transient failures and pending-request teardown.
It also accepts an exactly pinned self-signed host certificate, matching the
default Sunshine certificate model, without trusting it in the Windows store.
Sequence tests distinguish an explicit host re-copy of identical text from
the echo of a successful local POST, including copies made after non-text data.
Hosts without sequence headers retain content-based echo suppression.
Queued and in-flight POST tests verify that a newer file/image copy cancels
older text and its failed retry, while a newer text copy remains pending.
Guarded POST fixtures observe the initial host cursor before uploading, reject
a stale upload after a newer host copy, and preserve a newer pending local copy
while recovering from a conflict. They use the shared text/file coordinator and
check settlement, incoming-file blocking, duplicate observations, older task
completion, native sequence wrap and remote-publication echo suppression.
Fast successive local copies preserve the older successful POST's cursor, and
a POST that commits but loses its response is observed again before uploading
the newer pending copy. Authoritative text observations also override the file
worker's preliminary clipboard-format guesses.
Native publication callbacks compare the expected clipboard generation and
return the generation captured before releasing their write lock. Fixtures
simulate a file copy exactly between sampling and publication, a same-text
copy immediately after publication, and text/file copies during sampling;
stale text is neither published nor retried over those newer copies.
Startup non-text copies consume the first host baseline without applying its
pre-existing text, including hosts without sequence headers and torn initial
samples. Shared incoming publication tests also serialize concurrent text
observations, reject pending/stale generations and preserve publication identity.
An incoming file publication winning immediately after a stable text sample
also cancels the rejected snapshot without uploading stale text or echoing files.
The Windows writer prepares Unicode, ANSI, OEM and locale formats before
opening the clipboard. This prevents Windows from registering missing
conversions during close and advancing the captured publication generation.

`run_file_service_tests.ps1 -ServiceSourceRoot <Sunshine checkout>\src` compiles
the actual Sunshine file service with MSVC and Moonlight's existing OpenSSL
libraries. Its injected desktop callbacks leave the real clipboard alone.
It verifies ready unpublished stage cleanup on service shutdown, cleanup after
cancellation while the service remains alive, and revocation during asynchronous
verification preventing publication. This isolated module check complements
the full Sunshine build and real stream acceptance test.

`run_file_tests.ps1` runs the shared file engine without a Sunshine checkout or
Qt runtime. It checks bounded manifests (including 100,000 tiny-file entries),
batched file copies, empty/zero-byte files, local paths longer than 260 characters,
network/device/reparse path rejection, changed/corrupt/incomplete files,
cancellation and owned staging cleanup. Its local disk timing is a fixture
measurement, not a WAN throughput benchmark. It never reads the live clipboard.
`run_file_service_tests.ps1 -ServiceSourceRoot <Sunshine checkout>\src` also
exercises the actual private Windows impersonation scope
using an ephemeral restricted token and ACLs applied only to its new fixture
directory. It verifies denied reads cannot use the process's broader permission,
each asynchronous verification lane enters the effective user scope, scopes
restore the caller, and unavailable scopes fail before file I/O. It does not
alter a Windows account, the real clipboard or existing filesystem ACLs.
