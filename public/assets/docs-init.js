  // NOTE: custom themeVariables are only honored with theme:'base' in Mermaid
  // v10 — with theme:'dark' they are silently ignored (that was the source of
  // the unreadable khaki clusters).
  mermaid.initialize({
    startOnLoad: true,
    theme: 'base',
    themeVariables: {
      darkMode: true,
      fontSize: '16px',
      background: '#16191b',
      mainBkg: '#1d2327', primaryColor: '#1d2327', primaryTextColor: '#e9edef',
      primaryBorderColor: '#39434b', nodeBorder: '#39434b',
      secondaryColor: '#232a2f', tertiaryColor: '#12161a',
      lineColor: '#8b949b', textColor: '#e9edef', nodeTextColor: '#e9edef',
      edgeLabelBackground: '#0a0b0c',
      clusterBkg: '#111518', clusterBorder: '#2b3339', titleColor: '#e9edef',
      actorBkg: '#1d2327', actorBorder: '#22c86e', actorTextColor: '#e9edef',
      actorLineColor: '#5a646b', signalColor: '#aeb7bf', signalTextColor: '#e9edef',
      noteBkgColor: '#20282d', noteTextColor: '#cdd9e5', noteBorderColor: '#39434b',
      activationBorderColor: '#22c86e', activationBkgColor: '#12271b',
      sequenceNumberColor: '#0a0b0c', labelBoxBkgColor: '#1d2327',
      labelBoxBorderColor: '#39434b', labelTextColor: '#e9edef', loopTextColor: '#e9edef',
      fontFamily: '"Space Grotesk", -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif',
    },
    // useMaxWidth:false renders every diagram at its natural size — text stays
    // at the configured 16px and wide diagrams scroll inside .diagram-wrap
    // instead of shrinking to fit the column.
    flowchart: { curve: 'basis', htmlLabels: true, useMaxWidth: false, nodeSpacing: 55, rankSpacing: 60, padding: 12 },
    sequence: { useMaxWidth: false, actorMargin: 55, messageMargin: 28, width: 160,
      actorFontSize: 15, actorFontWeight: 600, messageFontSize: 14, noteFontSize: 13 },
    state: { useMaxWidth: false },
    class: { useMaxWidth: false },
  });

  const sections = document.querySelectorAll('section[id]');
  const navLinks = document.querySelectorAll('#sidebar a');
  const observer = new IntersectionObserver(entries => {
    entries.forEach(e => {
      if (e.isIntersecting) {
        navLinks.forEach(a => a.classList.remove('active'));
        const active = document.querySelector(`#sidebar a[href="#${e.target.id}"]`);
        if (active) active.classList.add('active');
      }
    });
  }, { rootMargin: '-20% 0px -70% 0px' });
  sections.forEach(s => observer.observe(s));
