"""Failure-atomic helpers for isolated Unreal import destinations."""


def run_isolated_import_transaction(operation, destination,
                                    does_directory_exist, delete_directory):
    """Run ``operation`` and remove its isolated destination on any failure.

    Unreal import APIs save intermediate assets before later verification. A
    failed stripped import must therefore delete the entire per-asset isolated
    directory, including any carrier-bearing SkeletalMesh already on disk.
    """
    try:
        return operation()
    except BaseException as import_error:
        try:
            if does_directory_exist(destination):
                deleted = delete_directory(destination)
                if deleted is False or does_directory_exist(destination):
                    raise RuntimeError(
                        f"isolated import cleanup did not remove {destination}")
        except BaseException as cleanup_error:
            raise RuntimeError(
                f"stripped import failed and cleanup of {destination} also "
                f"failed: {cleanup_error}") from import_error
        raise
