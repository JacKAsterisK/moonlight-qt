# Clipboard sync with the Sunshine fork

The forks support bidirectional UTF-8 text and Windows Explorer file/folder
copies during an active stream. Each feature is independently opt-in.

## Enable and use

1. Install the updated Sunshine fork on the Windows host. In its input settings,
   enable **Clipboard Text Sync** and/or **Clipboard File and Folder Transfer**,
   then restart Sunshine as requested. The corresponding configuration keys are
   `clipboard = enabled` and `clipboard_files = enabled`.
2. Install the updated Moonlight fork on the client. In **Settings → Input
   Settings**, enable **Synchronize text clipboard while streaming** and/or
   **Synchronize copied files and folders while streaming**.
3. Start a new stream. Copy text normally. For files, select files or folders in
   Explorer and use **Copy**, then paste on the other computer after the transfer
   has completed. Moonlight shows the file copy status during the transfer and
   **Files ready to paste** when it finishes. Files enter a temporary local stage first; the
   receiving Explorer clipboard is populated only after every file is verified.

File copying requires Windows at both ends. It copies file contents and directory
structure, including empty folders and zero-byte files. It does not preserve
permissions, timestamps, alternate data streams, links or other filesystem
metadata. Junctions and symbolic links are rejected. **Cut** does not delete the
source on the other computer. A newer clipboard copy cancels an unfinished
transfer so an older result cannot replace the new selection. Copy the selection
again to retry a failed transfer.

The first version accepts selections on local drives. UNC shares and mapped
network drives are rejected; copy their files to a local folder first. This keeps
an unavailable share from blocking transfer cancellation or stream shutdown.

Existing selections on the client are not uploaded when a stream starts. Copy
again during the stream to send them. Received copies remain available for
pasting after the stream ends. Staging uses local disk space, so a transfer needs
room for a complete extra copy on the receiving computer.
Old received stages are cleaned after seven days when they are no longer in the
clipboard. Sunshine stages files in the signed-in host user's local application
data directory and reads files with that user's permissions. Signing out or
changing the active host user stops that user's transfers. Other applications
running as the same Windows user can access these staged files.

## Transfer behavior

Text is limited to 1 MiB of UTF-8. File transfers support up to 100,000 entries and
64 GiB of contents, subject to a 32 MiB path manifest and a 32 MiB native
clipboard path list. For a very large selection of individual files, copying
their containing folder keeps the native path list small. Relative paths must be
valid, unambiguous Windows filenames. The transfer packs small files into 4 MiB
binary batches and uses up to four concurrent HTTPS connections, avoiding a
separate network round trip for each tiny file. Each file is checked with SHA-256
before clipboard publication. The first protocol version does not compress file
contents; actual speed depends on disk performance and the network.

Transfer work runs separately from Moonlight's rendering and input loop. The
existing paired HTTPS connection identity is used, with the exact paired host
certificate pinned. Sunshine also checks that the requesting client's certificate
belongs to the current active stream. Disabling or unpairing a client, disabling
the feature, or ending that stream prevents further authorized clipboard access.
The transfer uses Sunshine's existing HTTPS port; no additional listener or
firewall rule is needed.

Both forks are required. An upstream host without the advertised capability
continues streaming with clipboard sync unavailable. These settings take effect
on the next stream and are disabled by default.

## Verification

The text protocol integration fixture builds the actual Moonlight helper and
uses loopback HTTPS, ephemeral mutual TLS identities and in-memory clipboard
callbacks. It tests copying in both directions, echo suppression, stale replies,
certificate pinning, response bounds, clipboard contention and teardown without
touching the user's clipboard or saved pairing identity. See
`tests/clipboard/README.md` for commands.

For a real stream, test text in both directions, then copy a folder containing
many tiny files, an empty folder and a large file. Compare the resulting tree and
file hashes. Copy another selection during a transfer and confirm that only the
new selection becomes available. Stop and restart the stream, then repeat. Also
verify ordinary streaming with both options off.

## Existing work

[Apollo](https://github.com/ClassicOldSong/Apollo) provides an existing text
clipboard endpoint design. The forks use the paired HTTPS approach with bounded
payloads and request-specific stream authorization. The
[Moonlight clipboard proposal](https://github.com/moonlight-stream/moonlight-qt/pull/2019)
and its [Sunshine counterpart](https://github.com/LizardByte/Sunshine/pull/5782)
also informed the investigation. The Windows file transfer is a separate
implementation shared by these forks.

Windows Explorer clipboard formats are described in Microsoft's
[Shell clipboard documentation](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard).
