package main

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"golang.org/x/sys/unix"
)

func lockUpdate(path string) (func(), error) {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err := unix.Flock(int(f.Fd()), unix.LOCK_EX|unix.LOCK_NB); err != nil {
		f.Close()
		return nil, err
	}
	return func() { f.Close() }, nil
}

func obsRunning() (bool, error) {
	err := execQuiet("/usr/bin/pgrep", "-ix", "OBS")
	var exit *exec.ExitError
	if errors.As(err, &exit) && exit.ExitCode() == 1 {
		return false, nil
	}
	return err == nil, err
}

func validatePayload(current, next, version string) error {
	plist := filepath.Join(next, "Contents", "Info.plist")
	reported, err := exec.Command("/usr/libexec/PlistBuddy", "-c", "Print :CFBundleVersion", plist).Output()
	if err != nil || strings.TrimSpace(string(reported)) != version {
		return errors.New("Mac update bundle has the wrong version")
	}
	installedRequirement := "=anchor apple generic and identifier \"live.streamtranslate.obs-plugin\" and certificate leaf[subject.OU] = \"VJSDPHM64R\" and certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
	return execQuiet("/usr/bin/codesign", "--verify", "--deep", "--strict", "-R", installedRequirement, next)
}

func install(current, next, backup string) error {
	// The live path always points at a complete bundle, even if the updater
	// is killed during the swap.
	if err := unix.RenamexNp(current, next, unix.RENAME_SWAP); err != nil {
		return err
	}
	if err := keepPrevious(next, backup); err != nil {
		return errors.Join(err, unix.RenamexNp(current, next, unix.RENAME_SWAP))
	}
	return nil
}
