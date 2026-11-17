package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"unsafe"

	"golang.org/x/sys/windows"
)

func lockUpdate(path string) (func(), error) {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	flags := uint32(windows.LOCKFILE_EXCLUSIVE_LOCK | windows.LOCKFILE_FAIL_IMMEDIATELY)
	if err := windows.LockFileEx(windows.Handle(f.Fd()), flags, 0, 1, 0, &windows.Overlapped{}); err != nil {
		f.Close()
		return nil, err
	}
	return func() { f.Close() }, nil
}

func obsRunning() (bool, error) {
	snapshot, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return false, err
	}
	defer windows.CloseHandle(snapshot)
	var entry windows.ProcessEntry32
	entry.Size = uint32(unsafe.Sizeof(entry))
	for err = windows.Process32First(snapshot, &entry); err == nil; err = windows.Process32Next(snapshot, &entry) {
		process := strings.ToLower(windows.UTF16ToString(entry.ExeFile[:]))
		if process == "obs64.exe" || process == "obs32.exe" || process == "obs.exe" {
			return true, nil
		}
	}
	if errors.Is(err, windows.ERROR_NO_MORE_FILES) {
		return false, nil
	}
	return false, err
}

func validatePayload(current, next, version string) error {
	for _, rel := range []string{"obs-streamtranslate.dll", "cacert.pem", "bin/64bit/obs-streamtranslate.dll", "bin/64bit/cacert.pem", "obs-streamtranslate-updater.exe", "bin/64bit/obs-streamtranslate-updater.exe"} {
		if err := regularFile(filepath.Join(next, filepath.FromSlash(rel))); err != nil {
			return err
		}
	}
	if filepath.Base(current) != filepath.Base(next) {
		return errors.New("Windows update root does not match the installation")
	}
	return nil
}

func install(current, next, backup string) error {
	// Keep the old directory available if Windows refuses the replacement.
	// The running updater is in the user's OBS config directory.
	retired, err := os.MkdirTemp(filepath.Dir(current), ".streamtranslate-retired-")
	if err != nil {
		return err
	}
	old := filepath.Join(retired, filepath.Base(current))
	if err := os.Rename(current, old); err != nil {
		os.Remove(retired)
		return err
	}
	if err := os.Rename(next, current); err != nil {
		restore := os.Rename(old, current)
		if restore != nil {
			return errors.Join(err, fmt.Errorf("restore the previous version from %s: %w", old, restore))
		}
		os.Remove(retired)
		return err
	}
	if err := keepPrevious(old, backup); err != nil {
		return fmt.Errorf("update installed; previous version saved at %s: %w", old, err)
	}
	return os.Remove(retired)
}
