package android.webkit;
import android.content.Context;
public class WebView extends android.view.View {
  public WebView(Context c){}
  public WebSettings getSettings(){return null;}
  public void loadUrl(String u){}
  public boolean canGoBack(){return false;}
  public void goBack(){}
  public void addJavascriptInterface(Object o, String name){}
}
