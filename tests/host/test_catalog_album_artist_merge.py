from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
LIB = ROOT / "src" / "media" / "library"


def test_album_artist_is_backfilled_when_later_track_has_explicit_tag():
    with tempfile.TemporaryDirectory() as temp:
        temp_path = Path(temp)
        (temp_path / "esp_err.h").write_text(r"""
#pragma once
using esp_err_t = int;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_RESPONSE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NO_MEM 0x105
#define ESP_ERR_INVALID_STATE 0x106
inline const char *esp_err_to_name(esp_err_t) { return "host-error"; }
""")
        (temp_path / "esp_heap_caps.h").write_text(r"""
#pragma once
#include <cstdlib>
#include <cstddef>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline void *heap_caps_malloc(std::size_t n, unsigned) { return std::malloc(n); }
inline void *heap_caps_calloc(std::size_t n, std::size_t s, unsigned) { return std::calloc(n, s); }
inline void *heap_caps_realloc(void *p, std::size_t n, unsigned) { return std::realloc(p, n); }
inline void heap_caps_free(void *p) { std::free(p); }
""")
        (temp_path / "esp_log.h").write_text(r"""
#pragma once
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
""")
        (temp_path / "app_diag_config.h").write_text(r"""
#pragma once
#define APP_DIAG_BOOT_VERBOSE 0
#define APP_DIAG_DISCARDED_LOGI(...) ((void)0)
""")

        harness = r"""
#include <cassert>
#include <cstring>
#include "media_catalog_v2.h"
#include "media_index_store.h"
#include "media_metadata.h"

static void verify_album_artist_merge(int explicit_track_index)
{
    char paths[] = "/music/01.mp3\0/music/02.mp3";
    char *artist_list_0[] = {const_cast<char *>("Artist X")};
    char *artist_list_1[] = {const_cast<char *>("Artist X")};
    MediaMetadataBuildV2 metadata[2] = {};

    for (int i = 0; i < 2; ++i) {
        metadata[i].title = const_cast<char *>(i == 0 ? "Track One" : "Track Two");
        metadata[i].display_artist = const_cast<char *>("Artist X");
        metadata[i].album = const_cast<char *>("Album X");
        metadata[i].artists = i == 0 ? artist_list_0 : artist_list_1;
        metadata[i].artist_count = 1;
        metadata[i].metadata_flags =
            MEDIA_TRACK_META_SCANNED_V2 |
            MEDIA_TRACK_META_HAS_ARTIST_TAG_V2 |
            MEDIA_TRACK_META_HAS_ALBUM_TAG_V2;
    }

    if (explicit_track_index >= 0) {
        metadata[explicit_track_index].album_artist = const_cast<char *>("Artist X");
        metadata[explicit_track_index].metadata_flags |= MEDIA_TRACK_META_HAS_ALBUM_ARTIST_TAG_V2;
    }

    MediaIndexRecord records[2] = {};
    records[0].path_offset = 0;
    records[1].path_offset = sizeof("/music/01.mp3");
    records[0].format = MediaFormat::MP3;
    records[1].format = MediaFormat::MP3;
    records[0].file_size_bytes = 4096;
    records[1].file_size_bytes = 4096;
    records[0].metadata_build = &metadata[0];
    records[1].metadata_build = &metadata[1];

    MusicCatalogV2 catalog = {};
    const esp_err_t ret = media_catalog_v2_build_from_index_records(
        records, 2, paths, sizeof(paths), &catalog
    );
    assert(ret == ESP_OK);
    assert(catalog.track_count == 2);
    assert(catalog.album_count == 1);
    assert(catalog.tracks[0].album_id == catalog.tracks[1].album_id);

    if (explicit_track_index >= 0) {
        const uint32_t album_artist_id = catalog.albums[0].album_artist_id;
        assert(album_artist_id != MEDIA_CATALOG_INVALID_ID_V2);
        const char *name = media_catalog_v2_pool_str(
            &catalog, catalog.artists[album_artist_id].name_off
        );
        assert(name != nullptr && std::strcmp(name, "Artist X") == 0);
    } else {
        assert(catalog.albums[0].album_artist_id == MEDIA_CATALOG_INVALID_ID_V2);
    }

    media_catalog_v2_release(&catalog);
}

int main()
{
    // The first track has no Album Artist tag; the second one does.
    verify_album_artist_merge(1);
    // The reverse order must remain valid as well.
    verify_album_artist_merge(0);
    // No explicit Album Artist tag remains a valid, unassigned album entity.
    verify_album_artist_merge(-1);
}
"""
        (temp_path / "catalog_regression.cpp").write_text(harness)
        executable = temp_path / "catalog_regression"
        subprocess.run(
            [
                "g++", "-std=c++17", "-O1", "-fsanitize=undefined",
                "-fno-omit-frame-pointer",
                "-I", str(temp_path),
                "-I", str(LIB),
                str(temp_path / "catalog_regression.cpp"),
                str(LIB / "media_catalog_v2.cpp"),
                str(LIB / "media_groups_v2.cpp"),
                "-o", str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)


def test_catalog_recovery_never_bisects_and_discards_an_arbitrary_prefix_track():
    source = (LIB / "media_library.cpp").read_text(encoding="utf-8")
    assert "prefix_bad" not in source
    assert "prefix_ok" not in source
    assert "已保留上一次有效曲库" in source
    assert "media_catalog_v2_publish(&next_catalog, fallback_crc)" in source
    assert "case_insensitive != 0 ? case_insensitive : strcmp(path_a, path_b)" in source
    assert "entry.scan_artwork_changed" in source
    assert "existing_path_count = 0U" in source


if __name__ == "__main__":
    test_album_artist_is_backfilled_when_later_track_has_explicit_tag()
    test_catalog_recovery_never_bisects_and_discards_an_arbitrary_prefix_track()
    print("PASS: Album Artist merge and safe Catalog recovery")
