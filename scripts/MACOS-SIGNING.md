# macOS Developer ID release signing

The installer branding stays **StreamTranslate OBS Plugin**. Developer ID exposes
the enrolled signing identity in certificate details. This does not create an
App Store listing or change other apps on the Apple team.

## Local signing

1. Create separate Developer ID Application and Developer ID Installer certificates.
   Keep their private keys outside this repository, encrypted at rest.
2. Import those identities and Apple's Developer ID intermediate into a dedicated
   keychain. Make that keychain available while signing, restoring the previous
   search list afterwards. Do not change trust overrides or Gatekeeper settings.
3. Set `MACOS_APPLICATION_IDENTITY`, `MACOS_INSTALLER_IDENTITY`, `MACOS_TEAM_ID`,
   and optionally `MACOS_KEYCHAIN_PATH` to the dedicated keychain.
4. Run `bash scripts/macos-sign.sh dist/obs-streamtranslate.plugin` **before**
   packaging. Build the installer using the existing workflow.
5. Have the account holder create a dedicated Apple app-specific password for
   notarization. Store credentials with `xcrun notarytool store-credentials` in
   the dedicated keychain; never commit them or put them in logs.
6. Set `NOTARY_KEYCHAIN_PROFILE` and run
   `bash scripts/macos-notarize.sh out/StreamTranslate-OBS-Plugin.pkg`.
   Only an Accepted response, valid stapled ticket and accepted Gatekeeper
   assessment produce a distributable installer.
7. Test a fresh **browser download** on a Mac with no prior exception. Confirm
   the Installer UI, successful install and OBS filter loading. Do not disable
   quarantine or use Open Anyway to manufacture a passing result.

## GitHub Actions (requires explicit private-key transfer approval)

Signing is dormant until repository variable `MACOS_SIGNING_ENABLED=true`.
Before enabling it, configure:

Repository variables:
- `MACOS_TEAM_ID`
- `MACOS_APPLICATION_IDENTITY` (full Developer ID Application name)
- `MACOS_INSTALLER_IDENTITY` (full Developer ID Installer name)

Encrypted repository secrets:
- `MACOS_SIGNING_P12_BASE64`: password-protected export containing **only the two
  new StreamTranslate signing identities**, plus intermediate certificates.
  With OpenSSL 3 use `pkcs12 -export -legacy` for macOS import compatibility.
- `MACOS_SIGNING_P12_PASSWORD`
- `NOTARY_APPLE_ID`
- `NOTARY_APP_PASSWORD`: dedicated Apple app-specific password, not login password.

Never add signing secrets to forks, PR-triggered workflows, ordinary notes or
release assets. Signing is permitted only on `refs/heads/main`. The temporary
runner keychain and exported file are deleted in an always-running cleanup step.
When enabled, missing credentials or rejected notarization fail the Mac build;
they never silently fall back to unsigned output. With signing disabled, the
existing unsigned development build remains unchanged.

Offline tests: `python3 tests/macos-signing.test.py`.
Real Apple notarization and fresh-download installation are separate required
checks; passing offline tests is not proof those steps succeeded.
