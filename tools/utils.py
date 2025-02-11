import os
import subprocess
import sys

def get_changed_files_in_commit(commit):
    """
    Gets the files changed in the given commit.
    """
    return run_command([
        "git",
        "diff-tree",
        "--no-commit-id",
        "--name-only",
        "-r",
        commit
    ]).split()

def get_changed_files():
    """
    Gets the files changed in the staging area.
    """
    return run_command([
        "git",
        "diff",
        "--name-only"
    ]).split()

def get_commit():
    """
    Retrieves the base commit to diff.
    """
    if "PREUPLOAD_COMMIT" in os.environ:
        return os.environ["PREUPLOAD_COMMIT"]
    else:
        print("WARNING: Not running as a pre-upload hook. Assuming commit to check = 'HEAD'", file=sys.stderr)
        return "HEAD"

def cd_to_art_root():
    if "REPO_ROOT" in os.environ:
        repo_root = os.environ["REPO_ROOT"]
    else:
        this_path = os.path.dirname(os.path.realpath(__file__))
        repo_root = os.path.join(this_path, '..', '..')

    os.chdir(os.path.join(repo_root, "art"))

def run_command(command):
    """
    Runs the given command in a sub-process.
    """
    return subprocess.check_output(
        command,
        stderr=subprocess.STDOUT,
        universal_newlines=True
    )