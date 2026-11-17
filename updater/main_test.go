package main

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func fixtureArchive(t *testing.T, name string) []byte {
	t.Helper()
	var out bytes.Buffer
	zipper := zip.NewWriter(&out)
	file, err := zipper.Create(name)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := file.Write([]byte("new plugin")); err != nil {
		t.Fatal(err)
	}
	if err := zipper.Close(); err != nil {
		t.Fatal(err)
	}
	return out.Bytes()
}

func TestPrepareUpdate(t *testing.T) {
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	const assetName = "StreamTranslate-OBS-Update.zip"
	for _, fixture := range []struct {
		name, entry, installed string
		damage, tamper         bool
		wantError, wantUpdate  bool
	}{
		{name: "signed-archive", entry: "plugin/bin", installed: "0.1.16", wantUpdate: true},
		{name: "unsigned", entry: "plugin/bin", installed: "0.1.16", damage: true, wantError: true},
		{name: "modified-archive", entry: "plugin/bin", installed: "0.1.16", tamper: true, wantError: true},
		{name: "no-downgrade", entry: "plugin/bin", installed: "0.1.18"},
		{name: "same-version", entry: "plugin/bin", installed: "0.1.17"},
		{name: "path-traversal", entry: "../outside", installed: "0.1.16", wantError: true},
	} {
		t.Run(fixture.name, func(t *testing.T) {
			archive := fixtureArchive(t, fixture.entry)
			manifest, err := json.Marshal(manifest{"0.1.17", map[string]asset{
				assetName: {fmt.Sprintf("%x", sha256.Sum256(archive)), int64(len(archive))},
			}})
			if err != nil {
				t.Fatal(err)
			}
			signature := ed25519.Sign(private, manifest)
			if fixture.damage {
				signature[0] ^= 1
			}
			if fixture.tamper {
				archive[0] ^= 1
			}
			httpServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/latest/download/obs-update.json":
					w.Write(manifest)
				case "/latest/download/obs-update.sig":
					fmt.Fprint(w, base64.StdEncoding.EncodeToString(signature))
				case "/download/v0.1.17/" + assetName:
					w.Write(archive)
				default:
					http.NotFound(w, r)
				}
			}))
			defer httpServer.Close()
			stage := t.TempDir()
			bundle, version, err := prepareUpdate(httpServer.Client(), httpServer.URL+"/", stage,
				"plugin", assetName, versionParts(fixture.installed), public)
			if (err != nil) != fixture.wantError || (bundle != "") != fixture.wantUpdate {
				t.Fatalf("unexpected candidate %q, version %q, error %v", bundle, version, err)
			}
			if fixture.wantUpdate {
				data, err := os.ReadFile(filepath.Join(bundle, "bin"))
				if err != nil || string(data) != "new plugin" || version != "0.1.17" {
					t.Fatalf("incorrect staged file: %q, %q, %v", data, version, err)
				}
			}
		})
	}
}

func TestRequiresOptInAndOBSExit(t *testing.T) {
	choice := filepath.Join(t.TempDir(), "auto-updates.enabled")
	if err := waitForOBS(choice, func() (bool, error) {
		t.Fatal("checked OBS before consent")
		return false, nil
	}, time.Second); err == nil {
		t.Fatal("accepted absent consent")
	}
	if err := os.WriteFile(choice, nil, 0600); err != nil {
		t.Fatal(err)
	}
	if err := waitForOBS(choice, func() (bool, error) { return true, nil }, -1); err == nil {
		t.Fatal("accepted active OBS")
	}
	if err := waitForOBS(choice, func() (bool, error) { return false, nil }, time.Second); err != nil {
		t.Fatal(err)
	}
}

func TestInstallKeepsPreviousVersion(t *testing.T) {
	dir := t.TempDir()
	current, next, backup := filepath.Join(dir, "owned"), filepath.Join(dir, "next"), filepath.Join(dir, "previous")
	for folder, version := range map[string]string{current: "old", next: "new"} {
		if err := os.Mkdir(folder, 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(folder, "binary"), []byte(version), 0644); err != nil {
			t.Fatal(err)
		}
	}
	if err := install(current, next, backup); err != nil {
		t.Fatal(err)
	}
	for folder, version := range map[string]string{current: "new", backup: "old"} {
		got, err := os.ReadFile(filepath.Join(folder, "binary"))
		if err != nil || string(got) != version {
			t.Fatalf("incorrect installed or saved version: %q, %v", got, err)
		}
	}
}
