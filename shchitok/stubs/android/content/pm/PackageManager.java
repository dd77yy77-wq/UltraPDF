package android.content.pm;
public abstract class PackageManager {
  public static class NameNotFoundException extends Exception {}
  public abstract PackageInfo getPackageInfo(String pkg, int flags) throws NameNotFoundException;
  public abstract boolean canRequestPackageInstalls();
}
