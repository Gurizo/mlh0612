// EYE FIGHT — offline gate (Cloudflare Worker)
//
// Put this Worker ONLY on the landing-page routes (see DEPLOY below), NOT on
// /api/* — otherwise every camera frame would pass through the Worker and burn
// the free 100k-request/day quota. The page is the only thing a visitor hits
// when the server is off, so gating it is enough.
//
// Behavior: proxy the request to the origin (the Cloudflare Tunnel). If the
// origin is unreachable — the Mac isn't running go-live.sh, or it crashed —
// Cloudflare returns a 52x/530, or fetch throws; either way we serve a branded
// "arena is busy, try in a minute" page that auto-refreshes.
//
// DEPLOY (one time, in the Cloudflare dashboard):
//   1. Workers & Pages -> Create -> Worker -> name it "eyefight-gate" -> Deploy
//   2. Edit code -> paste this whole file -> Deploy
//   3. Worker -> Settings -> Domains & Routes -> Add route:
//        eyefight.xyz/          (zone: eyefight.xyz)
//        www.eyefight.xyz/      (zone: eyefight.xyz)
//      (Just the root path "/" — leave /api/*, /events, /avatar going direct.)
//
// To verify: stop go-live.sh, open https://eyefight.xyz -> you should see this
// page instead of a Cloudflare error.

export default {
  async fetch(request) {
    try {
      const resp = await fetch(request);
      if (resp.status >= 521 && resp.status <= 530) return offline();  // origin unreachable
      return resp;
    } catch (_) {
      return offline();
    }
  },
};

function offline() {
  return new Response(PAGE, {
    status: 503,
    headers: {
      "content-type": "text/html; charset=utf-8",
      "retry-after": "30",
      "cache-control": "no-store",
    },
  });
}

const PAGE = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta http-equiv="refresh" content="30">
<title>EYE FIGHT — be right back</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;600&family=Space+Grotesk:wght@700&display=swap" rel="stylesheet">
<style>
  :root { --bg:#0b0e14; --text:#e6edf3; --dim:#7d8aa5; --accent:#ff4d6d; --border:#232c44; }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { background:var(--bg); color:var(--text); font-family:'Inter',system-ui,sans-serif;
         min-height:100vh; display:flex; align-items:center; justify-content:center; padding:24px; text-align:center; }
  .bg { position:fixed; inset:0; z-index:-1; overflow:hidden; }
  .blob { position:absolute; border-radius:50%; filter:blur(80px); opacity:.25; }
  .b1 { width:340px; height:340px; background:var(--accent); top:-90px; left:-80px; }
  .b2 { width:300px; height:300px; background:#4dabff; bottom:-90px; right:-90px; }
  .wrap { max-width:420px; }
  h1 { font-family:'Space Grotesk',sans-serif; font-weight:700; font-size:clamp(30px,9vw,46px);
       letter-spacing:2px; margin-bottom:4px; }
  h1 .a { color:var(--accent); }
  .seal { width:108px; height:108px; margin:18px auto 22px; position:relative; }
  .seal svg { width:100%; height:100%; animation:spin 11s linear infinite; }
  .seal text { fill:var(--dim); font-size:9px; letter-spacing:2.5px; text-transform:uppercase; font-family:'Inter',sans-serif; }
  .seal .eye { position:absolute; inset:0; display:flex; align-items:center; justify-content:center; font-size:30px; }
  @keyframes spin { to { transform:rotate(360deg); } }
  .msg { font-size:18px; font-weight:600; margin-bottom:10px; }
  .sub { color:var(--dim); font-size:14px; line-height:1.5; }
  .pill { display:inline-block; margin-top:22px; padding:10px 20px; border-radius:999px;
          border:1px solid var(--border); color:var(--dim); font-size:13px; }
  button { margin-top:18px; padding:14px 28px; font:inherit; font-weight:700; font-size:16px; letter-spacing:1px;
           color:#fff; background:var(--accent); border:0; border-radius:10px; cursor:pointer; }
</style>
</head>
<body>
<div class="bg"><div class="blob b1"></div><div class="blob b2"></div></div>
<div class="wrap">
  <h1><span class="a">EYE</span> FIGHT</h1>
  <div class="seal">
    <svg viewBox="0 0 100 100"><defs><path id="c" d="M50,50 m-38,0 a38,38 0 1,1 76,0 a38,38 0 1,1 -76,0"/></defs>
      <text><textPath href="#c">TOO MANY DRY EYES • TOO MANY DRY EYES • </textPath></text></svg>
    <span class="eye">😵‍💫</span>
  </div>
  <div class="msg">the arena is catching its breath</div>
  <div class="sub">Too many dry eyes staring at once right now.<br>Hang tight — we'll be back in a minute.</div>
  <div class="pill">⏳ auto-refreshing every 30s</div>
  <div><button onclick="location.reload()">try now 👁</button></div>
</div>
</body>
</html>`;
