package main

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"time"
)

const (
	releaseURL = "https://github.com/StreamTranslate/obs-streamtranslate/releases/"
	maxArchive = 80 << 20
	maxExtract = 160 << 20
)

var publicKey string // Set at build time. A missing key disables the updater.

type asset struct {
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size"`
}

type manifest struct {
	Version string           `json:"version"`
	Assets  map[string]asset `json:"assets"`
}

var versionPattern = regexp.MustCompile(`^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$`)

func versionParts(version string) []int {
	if !versionPattern.MatchString(version) {
		return nil
	}
	parts := make([]int, 0, 3)
	for _, field := range strings.Split(version, ".") {
		number, _ := strconv.Atoi(field)
		parts = append(parts, number)
	}
	return parts
}

func verifyManifest(body, sig, key []byte) (manifest, error) {
	var update manifest
	signature, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(sig)))
	if err != nil || len(key) != ed25519.PublicKeySize || !ed25519.Verify(key, body, signature) {
		return update, errors.New("update manifest signature is invalid")
	}
	decoder := json.NewDecoder(bytes.NewReader(body))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&update); err != nil {
		return update, err
	}
	if decoder.Decode(new(any)) != io.EOF || versionParts(update.Version) == nil {
		return update, errors.New("malformed update manifest")
	}
	return update, nil
}

func checkRedirect(req *http.Request, via []*http.Request) error {
	if len(via) < 6 && req.URL.Scheme == "https" && req.URL.Port() == "" && req.URL.User == nil {
		switch req.URL.Hostname() {
		case "github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com":
			return nil
		}
	}
	return errors.New("unexpected update download redirect")
}

func fetch(client *http.Client, url string, max int64, dest io.Writer) error {
	response, err := client.Get(url)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK || response.ContentLength > max {
		return fmt.Errorf("update download HTTP status %d or size outside limit", response.StatusCode)
	}
	n, err := io.Copy(dest, io.LimitReader(response.Body, max+1))
	if err == nil && n > max {
		return errors.New("update download exceeded size limit")
	}
	return err
}

func extract(archive *zip.Reader, dest string) error {
	var total uint64
	for _, file := range archive.File {
		if !filepath.IsLocal(file.Name) || strings.ContainsAny(file.Name, `\:`) ||
			path.Clean(file.Name) != strings.TrimSuffix(file.Name, "/") {
			return errors.New("unsafe update archive path")
		}
		target := filepath.Join(dest, file.Name)
		if file.FileInfo().IsDir() {
			if err := os.MkdirAll(target, 0755); err != nil {
				return err
			}
			continue
		}
		if !file.Mode().IsRegular() || file.UncompressedSize64 > maxExtract-total {
			return errors.New("unsupported update archive entry")
		}
		total += file.UncompressedSize64
		if err := extractFile(file, target); err != nil {
			return err
		}
	}
	return nil
}

func extractFile(file *zip.File, target string) error {
	if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
		return err
	}
	in, err := file.Open()
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644|file.Mode().Perm()&0111)
	if err != nil {
		return err
	}
	n, copyErr := io.Copy(out, io.LimitReader(in, int64(file.UncompressedSize64)+1))
	closeErr := out.Close()
	if n != int64(file.UncompressedSize64) {
		return errors.New("unexpected extracted file size")
	}
	return errors.Join(copyErr, closeErr)
}

func installedTarget(binary string) (target, assetName string, err error) {
	if !filepath.IsAbs(binary) {
		return "", "", errors.New("missing absolute plugin path")
	}
	canonical, err := filepath.EvalSymlinks(binary)
	if err != nil {
		return "", "", err
	}
	switch runtime.GOOS {
	case "darwin":
		current := filepath.Dir(filepath.Dir(filepath.Dir(canonical)))
		if filepath.Base(current) == "obs-streamtranslate.plugin" &&
			canonical == filepath.Join(current, "Contents", "MacOS", "obs-streamtranslate") {
			return current, "StreamTranslate-OBS-Update-macOS-arm64.zip", nil
		}
	case "windows":
		dir := filepath.Dir(canonical)
		if filepath.Base(dir) == "64bit" && filepath.Base(filepath.Dir(dir)) == "bin" {
			dir = filepath.Dir(filepath.Dir(dir))
		}
		if strings.EqualFold(filepath.Base(dir), "obs-streamtranslate") &&
			strings.EqualFold(filepath.Base(canonical), "obs-streamtranslate.dll") {
			return dir, "StreamTranslate-OBS-Update-Windows-x64.zip", nil
		}
	}
	return "", "", errors.New("this plugin install layout requires manual updates")
}

func optedIn(choice string) bool {
	info, err := os.Stat(choice)
	return err == nil && info.Mode().IsRegular()
}

func waitForOBS(choice string, running func() (bool, error), timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		if !optedIn(choice) {
			return errors.New("automatic updates were turned off")
		}
		active, err := running()
		if err != nil {
			return err
		}
		if !active {
			return nil
		}
		if time.Now().After(deadline) {
			return errors.New("OBS did not exit before the update deadline")
		}
		time.Sleep(time.Second)
	}
}

func run(binary, choice, installed string) error {
	key, err := base64.StdEncoding.DecodeString(publicKey)
	if err != nil || len(key) != ed25519.PublicKeySize || !optedIn(choice) {
		return nil
	}
	current, name, err := installedTarget(binary)
	if err != nil {
		return err
	}
	oldParts := versionParts(installed)
	if oldParts == nil {
		return errors.New("invalid installed version")
	}

	// A temporary directory outside the plugin prevents OBS from scanning it.
	parent := filepath.Dir(current)
	releaseLock, err := lockUpdate(filepath.Join(parent, ".streamtranslate-update.lock"))
	if err != nil {
		return err
	}
	defer releaseLock()
	before, err := os.Stat(binary)
	if err != nil {
		return err
	}
	stage, err := os.MkdirTemp(parent, ".streamtranslate-update-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	client := &http.Client{Timeout: 2 * time.Minute, CheckRedirect: checkRedirect}
	next, version, err := prepareUpdate(client, releaseURL, stage, filepath.Base(current), name, oldParts, key)
	if err != nil || next == "" {
		return err
	}
	if err := validatePayload(current, next, version); err != nil {
		return err
	}
	if err := waitForOBS(choice, obsRunning, 24*time.Hour); err != nil {
		return err
	}
	after, err := os.Stat(binary)
	if err != nil || !os.SameFile(before, after) || before.Size() != after.Size() ||
		!before.ModTime().Equal(after.ModTime()) {
		return errors.New("the installed plugin changed while the update was staged")
	}
	active, err := obsRunning()
	if err != nil || active || !optedIn(choice) {
		return errors.New("OBS reopened or automatic updates were turned off")
	}
	return install(current, next, filepath.Join(parent, ".streamtranslate-previous"))
}

func prepareUpdate(client *http.Client, release, stage, folder, name string, oldParts []int, key []byte) (string, string, error) {
	var metadata, signature bytes.Buffer
	if err := fetch(client, release+"latest/download/obs-update.json", 32768, &metadata); err != nil {
		return "", "", err
	}
	if err := fetch(client, release+"latest/download/obs-update.sig", 256, &signature); err != nil {
		return "", "", err
	}
	update, err := verifyManifest(metadata.Bytes(), signature.Bytes(), key)
	if err != nil {
		return "", "", err
	}
	if slices.Compare(versionParts(update.Version), oldParts) <= 0 {
		return "", "", nil
	}
	payload, ok := update.Assets[name]
	if !ok || payload.Size <= 0 || payload.Size > maxArchive {
		return "", "", errors.New("release has no matching desktop update")
	}
	digest, err := hex.DecodeString(payload.SHA256)
	if err != nil || len(digest) != sha256.Size {
		return "", "", errors.New("release has an invalid archive digest")
	}
	archivePath := filepath.Join(stage, "update.zip")
	if err := getArchive(client, release+"download/v"+update.Version+"/"+name, archivePath, payload.Size, digest); err != nil {
		return "", "", err
	}
	archive, err := zip.OpenReader(archivePath)
	if err != nil {
		return "", "", err
	}
	extractErr := extract(&archive.Reader, stage)
	closeErr := archive.Close()
	if err := errors.Join(extractErr, closeErr); err != nil {
		return "", "", err
	}
	return filepath.Join(stage, folder), update.Version, nil
}

func getArchive(client *http.Client, url, destination string, size int64, digest []byte) error {
	f, err := os.OpenFile(destination, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	hash := sha256.New()
	downloadErr := fetch(client, url, size, io.MultiWriter(f, hash))
	stat, statErr := f.Stat()
	closeErr := f.Close()
	if err := errors.Join(downloadErr, statErr, closeErr); err != nil {
		return err
	}
	if stat.Size() != size || !bytes.Equal(hash.Sum(nil), digest) {
		return errors.New("update archive does not match the signed release")
	}
	return nil
}

func regularFile(file string) error {
	info, err := os.Lstat(file)
	if err == nil && !info.Mode().IsRegular() {
		return fmt.Errorf("%s is not a regular file", filepath.Base(file))
	}
	return err
}

func main() {
	if len(os.Args) != 4 {
		return
	}
	binary, choice, installed := os.Args[1], os.Args[2], os.Args[3]
	if !optedIn(choice) || publicKey == "" {
		return
	}
	if err := run(binary, choice, installed); err != nil {
		// The plugin UI can surface actionable errors after the next start.
		logFile, openErr := os.OpenFile(filepath.Join(filepath.Dir(choice), "desktop-update.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
		if openErr == nil {
			defer logFile.Close()
			log.New(logFile, "", log.LstdFlags).Print(err)
		}
	}
}

// execQuiet is used for system tools with absolute paths and fixed arguments.
func execQuiet(program string, args ...string) error {
	return exec.Command(program, args...).Run()
}

// The prior version lives outside OBS's plugin search paths.
func keepPrevious(old, backup string) error {
	if err := os.RemoveAll(backup); err != nil {
		return err
	}
	return os.Rename(old, backup)
}
