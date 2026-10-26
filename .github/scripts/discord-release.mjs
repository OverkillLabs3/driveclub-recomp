// Posts a release to the #releases channel of the Driveclub Native Runtime Discord.
// Run by .github/workflows/discord-release.yml when a release is published.
//
// Env:
//   DISCORD_WEBHOOK_URL  required unless DRY_RUN=1
//   RELEASE_TAG          tag to post (default: latest release)
//   RELEASE_PING_ROLE    Discord role id to ping (optional)
//   GITHUB_TOKEN         optional, raises the GitHub API rate limit
//   DRY_RUN=1            print a summary instead of posting

const REPO = process.env.GITHUB_REPOSITORY || "OverkillLabs3/driveclub-recomp";
const CHANNELS = {
  startHere: "1563589651871039712",
  setupHelp: "1563588370347855932",
};
const DESCRIPTION_LIMIT = 4000;

async function gh(path) {
  const headers = { Accept: "application/vnd.github+json", "User-Agent": "driveclub-release-bot" };
  if (process.env.GITHUB_TOKEN) headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`;
  const res = await fetch(`https://api.github.com/repos/${REPO}/${path}`, { headers });
  if (!res.ok) throw new Error(`GitHub ${path}: ${res.status} ${await res.text()}`);
  return res.json();
}

// Text between a heading line and the next heading of the same or higher level.
function section(body, heading) {
  const lines = body.split(/\r?\n/);
  const start = lines.findIndex((l) => l.trim() === heading);
  if (start === -1) return "";
  const level = heading.match(/^#+/)[0].length;
  const out = [];
  for (const line of lines.slice(start + 1)) {
    const m = line.match(/^(#+)\s/);
    if (m && m[1].length <= level) break;
    out.push(line);
  }
  return out.join("\n").trim();
}

// Discord embeds render headings inconsistently, so sub-headings become bold lines.
const tidy = (text) => text.replace(/^#{3,6}\s+(.*)$/gm, "**$1**").replace(/\n{3,}/g, "\n\n");

function clip(text, limit, url) {
  if (text.length <= limit) return text;
  const more = `\n\n… [Read the full release notes](${url})`;
  const cut = text.slice(0, limit - more.length);
  return cut.slice(0, cut.lastIndexOf("\n")) + more;
}

function buildPayload(release) {
  const version = release.tag_name.replace(/^v/, "");
  const body = release.body || "";
  const highlights = section(body, `## Highlights of ${version}`);
  const changes = section(body, `### ${version}`);

  let description = "";
  if (highlights) description += tidy(highlights) + "\n\n";
  if (changes) description += `**What changed in ${version}**\n` + tidy(changes);
  if (!description) description = tidy(body);
  description = clip(description.trim(), DESCRIPTION_LIMIT, release.html_url);

  const zip = release.assets.find((a) => /win64\.zip$/i.test(a.name)) || release.assets[0];
  const sha = zip && (body.match(new RegExp("`([0-9a-f]{64})`\\s+`" + zip.name.replace(/\./g, "\\.") + "`")) || [])[1];

  const fields = [];
  if (zip) fields.push({ name: "Download", value: `[${zip.name}](${zip.browser_download_url}) (${Math.round(zip.size / 1048576)} MB)` });
  if (sha) fields.push({ name: "SHA-256", value: "`" + sha + "`" });
  fields.push({
    name: "New here?",
    value: `Read <#${CHANNELS.startHere}>. Stuck? <#${CHANNELS.setupHelp}>. Found a bug? [Report it on GitHub](https://github.com/${REPO}/issues/new/choose) with your logs.`,
  });

  const role = process.env.RELEASE_PING_ROLE;
  return {
    username: "Driveclub Releases",
    avatar_url: "https://github.com/OverkillLabs3.png",
    content: `${role ? `<@&${role}> ` : ""}🏁 **${release.name || release.tag_name}** is out!`,
    allowed_mentions: { parse: [], roles: role ? [role] : [] },
    embeds: [{
      title: release.name || release.tag_name,
      url: release.html_url,
      description,
      color: 0xe74c3c,
      author: { name: REPO, url: `https://github.com/${REPO}`, icon_url: "https://github.com/OverkillLabs3.png" },
      fields,
      footer: { text: "Bring your own legally dumped copy of DriveClub v1.28. No game files are shared here." },
      timestamp: release.published_at,
    }],
  };
}

async function main() {
  const tag = process.env.RELEASE_TAG;
  const release = await gh(tag ? `releases/tags/${encodeURIComponent(tag)}` : "releases/latest");
  const payload = buildPayload(release);

  if (process.env.DRY_RUN) {
    console.log(`[dry run] ${release.tag_name}: ${payload.embeds[0].description.length} chars of notes, ${payload.embeds[0].fields.length} fields`);
    return;
  }

  const url = process.env.DISCORD_WEBHOOK_URL;
  if (!url) throw new Error("DISCORD_WEBHOOK_URL is not set. Add the DISCORD_RELEASES_WEBHOOK repository secret.");
  const res = await fetch(`${url}?wait=true`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  if (!res.ok) throw new Error(`Discord: ${res.status} ${await res.text()}`);
  console.log(`Posted ${release.tag_name} to Discord`);
}

main().catch((err) => { console.error(err.message); process.exit(1); });
