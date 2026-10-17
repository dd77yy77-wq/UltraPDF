package android.app;
import android.os.Bundle;
import android.content.Intent;
public class Activity extends android.content.Context {
  protected void onCreate(Bundle b){}
  protected void onResume(){}
  public void onBackPressed(){}
  public void setContentView(android.view.View v){}
  public android.view.Window getWindow(){return null;}
  public final void runOnUiThread(Runnable r){}
  public void startActivity(Intent i){}
  public boolean isFinishing(){return false;}
  public android.content.pm.PackageManager getPackageManager(){return null;}
  public String getPackageName(){return null;}
  public java.io.File getCacheDir(){return null;}
}
