# Android signing

The release certificate is `release-cert.der`; its SHA-256 fingerprint is in `release-cert-sha256.txt`. These files are public. Keep the corresponding private key for all future releases.

Release builds require a private Java-properties file, selected by `-SigningProperties`, `HDRBRIDGE_ANDROID_SIGNING_PROPERTIES`, or `android/.local-signing/release.properties`:

```properties
storeFile=C:/secure/hdrbridge/release.p12
storePassword=<password>
keyAlias=hdrbridge
keyPassword=<password>
```

Use absolute paths with forward slashes. Java-properties escaping applies to passwords and paths. Keep the key and passwords in a backed-up private location outside build and source directories. Do not regenerate the key for a new version. A lost release key can prevent installed apps from receiving updates.

Release APKs are verified across every supported Android API level (26–36) against this same production certificate. The app's embedded certificate pin (`RELEASE_CERT_SHA256`) and installed signer check reject updates signed with a different key.

## Release workflow

The Android release workflow runs after the Windows release job, or manually for an already published stable tag. It uploads the production-signed `HDRBridge-v<version>-Android-arm64.apk` and `SHA256SUMS-Android.txt`. Development builds use a `-preview.apk` filename and are not published as updates.

Configure these repository Actions secrets to enable signed CI releases:

- `ANDROID_RELEASE_KEYSTORE_BASE64`
- `ANDROID_RELEASE_STORE_PASSWORD`
- `ANDROID_RELEASE_KEY_ALIAS`
- `ANDROID_RELEASE_KEY_PASSWORD`

The keystore Base64 value is decoded only into the runner's temporary directory. Without these secrets, Android release publishing is skipped; Windows publishing is independent.
