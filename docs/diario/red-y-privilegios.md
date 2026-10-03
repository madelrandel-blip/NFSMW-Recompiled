# Multiplayer: how far it gets and where it stops

Short status: **it doesn't work.** The door is open; behind it, half a network layer is
missing.

This entry documents what was tried and what was measured, so that whoever wants to
attempt it doesn't repeat the path.

## Wall 1: privileges — solved

**Symptom:** on entering multiplayer, the game puts up a message and won't let you through.

> ATTENTION
> Your Xbox Live privileges do not allow you to access this feature.

It's not a bug or a hang: it's a clean **no**, and it comes long before the network is
touched.

It's in `src/kernel/xam/xam_user.cpp`, and the original comment leaves no doubt:

```cpp
u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value) {
  ...
  // If we deny everything, games should hopefully not try to do stuff.
  *out_value = 0;
  return X_ERROR_SUCCESS;
}
```

It denies **all** privileges, always, no matter which one is asked about. It comes from
Xenia and for an emulator without Xbox Live it makes sense: if the game believes it has
no permissions, it won't even try, and it won't hang on dead servers.

The striking thing is that the rest of the SDK says otherwise:

| Function | Returns |
|---|---|
| `XamUserIsOnlineEnabled` | 1 — there is a connection |
| `XamUserGetMembershipTier` | 6 — which is Gold |
| `user_profile.signin_state` | 1 — session signed in |
| `user_profile.type` | 1 \| 2 — local and online profile |

In other words: profile set up, session signed in, Gold membership, and zero permissions.
The only piece that said no was that one.

`parche_privilegios.py` adds `grant_user_privileges`, off by default. Turned on, it gets
past the message. **Confirmed working.**

## Wall 2: EA's servers — insurmountable

Once past the message, the game starts searching for matches on EA's official servers
and gets stuck in a loop.

This has no fix and doesn't deserve effort: those servers have been down for years. No
patch can bring them back. **The only possible route is System Link**, which doesn't use
EA infrastructure at all — two machines talking directly.

A useful thing ruled out: **it doesn't get stuck resolving the name.** `XNetDnsLookup` in
this SDK already fails fast on purpose:

```cpp
dns->status = 1;  // non-zero = error
if (event_handle) ev->Set(0, false);
```

It returns an error and wakes the event immediately. The loop is further up, probably
in the game retrying the session search indefinitely (see below).

## Wall 3: the XNet layer — the real work

Here is the heart of the matter. Cross-referencing the SDK ordinal table
(`src/kernel/xam/export_table.inc`) with what `xam_net.cpp` actually implements:

- **158** network functions declared
- **44** implemented
- **114** unimplemented

What's there works and it's not little: real sockets (`socket`, `bind`, `connect`,
`send`/`recv`, `sendto`/`recvfrom`, `select`), `XNetGetTitleXnAddr` returning the local
IP, `XNetSetSystemLinkPort`.

What's missing is precisely System Link:

| Ordinal | Function | What for |
|---|---|---|
| 0x36 | `XNetCreateKey` | create the match's XNKID/XNKEY |
| 0x37 | `XNetRegisterKey` | associate it on the other end |
| 0x38 | `XNetUnregisterKey` | |
| 0x3F | `XNetUnregisterInAddr` | |
| 0x41 | `XNetConnect` | bring up the link with the peer |
| 0x42 | `XNetGetConnectStatus` | know whether it came up |
| 0x53 | `XNetGetSystemLinkPort` | only the *setter* exists |
| 0x09 | `getsockname` | basic, and it's missing too |

That trio `CreateKey`/`RegisterKey`/`UnregisterKey` is what associates the session key.
Without it, the game doesn't even get to try to talk.

## Wall 4: the sessions are just for show

And there's a second layer just as empty. The session handlers in
`src/kernel/xam/apps/xgi_app.cpp` read the parameters, write them to the log and
return `X_E_SUCCESS` **without doing anything**. For example `XSessionSearch` (message
`0x000B0016`):

```cpp
REXKRNL_DEBUG("XSessionSearch({}, {}, {}, ...)", ...);
return X_E_SUCCESS;
```

It doesn't even touch the results buffer. A client searching for matches will always
find zero — and since it's told "success", it has no reason to give up. **A stub that
lies is worse than one that fails.** It's the most likely explanation for the loop in
wall 2.

Same with `XSessionCreate`, `XSessionJoinRemote`, `XSessionModify` and the rest.

## What it would take

In order, and without fooling yourself about the scale:

1. **The missing XNet stubs**, mapping XNADDR ↔ IP directly instead of emulating the
   real secure association. That's what Xenia's network forks do.
2. **Real match discovery**: for `XSessionCreate` to register a local session and for
   `XSessionSearch` to announce it and find it via UDP broadcast.
3. Only then, the UI.

## A design note on "send me your IP"

The natural idea —a panel with your IP and port so a friend can paste them as
"Host"— **is not how System Link works.** The 360 discovers matches via *broadcast* on
the local network, and a broadcast doesn't cross the internet.

To play with someone outside you'd need either a VPN that puts you both on the same
virtual LAN (ZeroTier, Radmin, Hamachi), or a direct-connection layer that replaces
broadcast with a specific IP. The latter is what XLink Kai and Xenia's network builds
do, and it's new design, not a tweak.

## The next step, if someone picks it up

`LOG_DETALLADO.bat` starts with `--log_level=debug --log_noisy=true`, and the session
handlers already write every call. So entering multiplayer with that on and letting it
spin for 30 seconds gives the exact sequence —what the game asks for, in what order and
where it repeats— without writing a line of code.

That log is the first thing to look at. Most Wanted may use quite a bit less than the
total that's missing.
