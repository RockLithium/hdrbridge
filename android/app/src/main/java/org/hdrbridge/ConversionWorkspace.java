package org.hdrbridge;

import java.io.File;
import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import org.json.JSONException;
import org.json.JSONObject;

/** Owns one exclusive app-cache directory, never the user-selected destination. */
final class ConversionWorkspace implements AutoCloseable {
  private static final String PREFIX = "hdr_job_";
  private final Path directory;
  private final File output;

  private ConversionWorkspace(Path directory, String extension) {
    this.directory = directory;
    output = directory.resolve("result" + extension).toFile();
  }

  static ConversionWorkspace create(File cacheRoot, String extension) throws IOException {
    if (extension == null || !extension.matches("\\.[A-Za-z0-9]{1,8}"))
      throw new IllegalArgumentException("Invalid internal output extension");
    // Exclusive allocation makes the replace/rename commit safe: this directory
    // cannot contain a previous output or the user's original image.
    return new ConversionWorkspace(Files.createTempDirectory(cacheRoot.toPath(), PREFIX), extension);
  }

  File output() { return output; }

  JSONObject nativeOptions(JSONObject requested) throws JSONException {
    JSONObject scoped = new JSONObject(requested.toString());
    // Only the core's private intermediate file uses rename/replace. SAF output
    // naming below remains a separate operation and never inherits this flag.
    scoped.put("overwrite", true);
    return scoped;
  }

  @Override public void close() throws IOException { deleteOwnedTree(directory); }

  static void cleanupAbandoned(File cacheRoot) throws IOException {
    File[] children = cacheRoot.listFiles();
    if (children == null) return;
    for (File child : children) {
      if (child.getName().startsWith(PREFIX) &&
          Files.isDirectory(child.toPath(), LinkOption.NOFOLLOW_LINKS))
        deleteOwnedTree(child.toPath());
    }
  }

  private static void deleteOwnedTree(Path directory) throws IOException {
    if (!Files.exists(directory, LinkOption.NOFOLLOW_LINKS)) return;
    // walkFileTree does not follow symlinks. Cleanup stays within the job that
    // this instance reserved, including any uncommitted core staging file.
    Files.walkFileTree(directory, new SimpleFileVisitor<Path>() {
      @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
        Files.delete(file);
        return FileVisitResult.CONTINUE;
      }
      @Override public FileVisitResult postVisitDirectory(Path dir, IOException error) throws IOException {
        if (error != null) throw error;
        Files.delete(dir);
        return FileVisitResult.CONTINUE;
      }
    });
  }
}
