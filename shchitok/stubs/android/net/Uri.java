package android.net;
public abstract class Uri implements android.os.Parcelable {
  public static Uri parse(String s){return null;}
  public abstract String getLastPathSegment();
}
