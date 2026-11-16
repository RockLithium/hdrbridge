package org.hdrbridge;

import static org.junit.Assert.*;

import java.io.File;
import java.nio.file.Files;
import org.json.JSONObject;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class ConversionWorkspaceTest {
  @Rule public TemporaryFolder temp = new TemporaryFolder();

  @Test
  public void jobsUseSeparatePrivateOutputsAndCloseOnlyTheirOwnFiles() throws Exception {
    File cache = temp.newFolder("cache");
    File source = new File(cache, "hdr_source_keep");
    Files.write(source.toPath(), new byte[] {1});
    try (ConversionWorkspace first = ConversionWorkspace.create(cache, ".png");
         ConversionWorkspace second = ConversionWorkspace.create(cache, ".png")) {
      File a = first.output();
      File b = second.output();
      assertNotEquals(a.getParentFile(), b.getParentFile());
      assertEquals(cache.getCanonicalFile(), a.getParentFile().getParentFile().getCanonicalFile());
      assertEquals(cache.getCanonicalFile(), b.getParentFile().getParentFile().getCanonicalFile());
      assertFalse(a.exists());
      assertFalse(b.exists());
      Files.write(a.toPath(), new byte[] {2});
      File partial = new File(a.getParentFile(), "partial");
      Files.write(partial.toPath(), new byte[] {3});
      Files.write(b.toPath(), new byte[] {4});
      first.close();
      assertFalse(a.getParentFile().exists());
      assertTrue(b.exists());
      assertTrue(source.exists());
    }
    assertTrue(source.exists());
  }

  @Test
  public void nativeOptionsCloneOverridesOnlyInternalOverwrite() throws Exception {
    JSONObject requested = new JSONObject().put("overwrite", false)
        .put("mode", "gainmap-extract").put("gainmap_export_format", "png");
    try (ConversionWorkspace workspace = ConversionWorkspace.create(temp.newFolder("cache"), ".png")) {
      JSONObject actual = workspace.nativeOptions(requested);
      assertNotSame(requested, actual);
      assertFalse(requested.getBoolean("overwrite"));
      assertTrue(actual.getBoolean("overwrite"));
      assertEquals("gainmap-extract", actual.getString("mode"));
      assertEquals("png", actual.getString("gainmap_export_format"));
    }
  }

  @Test
  public void startupCleanupRemovesAbandonedJobButPreservesOtherCacheFiles() throws Exception {
    File cache = temp.newFolder("cache");
    File source = new File(cache, "hdr_source_keep");
    Files.write(source.toPath(), new byte[] {1});
    File neighbor = new File(cache, "other_directory");
    assertTrue(neighbor.mkdir());
    File keep = new File(neighbor, "keep");
    Files.write(keep.toPath(), new byte[] {2});
    ConversionWorkspace abandoned = ConversionWorkspace.create(cache, ".jpg");
    File directory = abandoned.output().getParentFile();
    Files.write(abandoned.output().toPath(), new byte[] {3});
    Files.write(new File(directory, "partial").toPath(), new byte[] {4});

    ConversionWorkspace.cleanupAbandoned(cache);

    assertFalse(directory.exists());
    assertTrue(source.exists());
    assertTrue(keep.exists());
  }

  @Test
  public void rejectsExtensionsContainingPaths() throws Exception {
    File cache = temp.newFolder("cache");
    for (String extension : new String[] {"/escape", "../escape", "\\escape", ".png/other"}) {
      try {
        ConversionWorkspace workspace = ConversionWorkspace.create(cache, extension);
        workspace.close();
        fail("Accepted path extension: " + extension);
      } catch (IllegalArgumentException expected) {
        // Only a filename extension may be appended to the output name.
      }
    }
  }
}
