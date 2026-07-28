import sys

with open("ui/react.js", "r", encoding="utf-8") as f:
    react_js = f.read()
with open("ui/react-dom.js", "r", encoding="utf-8") as f:
    reactdom_js = f.read()

with open("ui/index.html", "r", encoding="utf-8") as f:
    html = f.read()

# Replace CDN/local script tags with inline React
html = html.replace(
    '<script src="react.js"></script>',
    '<script>' + react_js + '</script>'
)
html = html.replace(
    '<script src="react-dom.js"></script>',
    '<script>' + reactdom_js + '</script>'
)

with open("webui/index.html", "w", encoding="utf-8") as f:
    f.write(html)

print(f"Built webui/index.html ({len(html)} bytes)")
