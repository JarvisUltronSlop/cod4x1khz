# Build 70 SDK fixture sequence

This is a prepared sequence, with no native fixture launch or new semantic proof. Build 70 is frozen at `results/build70-source-and-binary-hashes.json`; its 39 native suites passed. The build owner must grant the exclusive fixture window after bot/UI checks and the four timing cases. The commands below never build or deploy.

Run serially and stop after any nonzero exit, failed case, mismatched archive or missing owned-process cleanup:

```powershell
python scripts/plugin_identity_format_native_probe.py --seconds 30 --build-archive results/build70-source-and-binary-hashes.json
python scripts/plugin_net_native_probe.py --seconds 15 --build-archive results/build70-source-and-binary-hashes.json
python scripts/plugin_udp_native_probe.py --build-archive results/build70-source-and-binary-hashes.json
```

The new formatter harness chooses two free ports starting at 28986 whose benchmark home directories do not exist. It does not erase existing homes or run `server.cfg`, and records the exact startup arguments. The first server has no `sv_usesteam64id` override and must report initial true. The second has startup `+set sv_usesteam64id 0` and must retain false through native registration. Each actual C DLL command checks 99 selected-format cases, 96 explicit Steam64 cases, and 128 Steam3 account/universe cases, with zero failures. It restores its original dvar setting and calls the existing independent parser/GUID oracle after restoration. Both modules must unload and both exact owned PIDs must exit. The 30-second bound includes cleanup.

The fresh-home fixture does not verify archived-setting persistence through a saved-config restart. That dvar obligation remains pending. Identifier conversion never authenticates an official Steam or CoD4x provider. Parser aliases, explicit instances and malformed-input adaptations need their own complete assertions before the remaining four conversion exports are promoted.

The address fixture uses server/headless-human ports 28982/28983. It must report 40 address checks and eight IPv4-ban checks, then prove actual private-slot admission, address conversion for native callbacks and unload. The UDP fixture uses server 28982 and listeners 28984/28985, with a 35-second bound. It must prove actual DNS, native source port, wire bytes, callbacks, veto, copied injection and retirement. IPv4-only transport/ban/callback entries remain partial; these fixtures do not establish native IPv6 transport.

After both network fixtures pass, use their exact printed result paths:

```powershell
python scripts/record_plugin_network_evidence.py --archive results/build70-source-and-binary-hashes.json --net-result <exact-network-result.json> --udp-result <exact-udp-result.json>
```

This prepares immutable Build 70 evidence and a candidate for the previously reviewed ten complete address/DNS entries plus nine partial network entries. It checks archive/current bytes and the actual address/DNS suite log. Review the candidate, then repeat the same command with `--publish`. Publication must preserve unrelated capability entries and historical Build 64 artifacts.

`config/staged/build70-sdk-identity-contracts.json` is a separate draft for the two formatter exports and the partially covered dvar. It contains no attestations or invented hashes and must remain outside the release evidence configuration until the loaded-DLL boundary fixtures pass and root reviews their exact assertions. The new formatter harness passed an AST syntax check; it has not launched a game.
