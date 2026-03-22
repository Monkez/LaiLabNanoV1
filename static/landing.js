/* ============================================================
   LaiLab Nano V1 — Landing Page Scripts
   ============================================================ */

document.addEventListener('DOMContentLoaded', () => {
    // ---- Theme Toggle ----
    const themeToggle = document.getElementById('themeToggle');
    const savedTheme = localStorage.getItem('lailab_theme') || 'dark';
    document.documentElement.setAttribute('data-theme', savedTheme);

    if (themeToggle) {
        themeToggle.addEventListener('click', () => {
            const current = document.documentElement.getAttribute('data-theme');
            const next = current === 'dark' ? 'light' : 'dark';
            document.documentElement.setAttribute('data-theme', next);
            localStorage.setItem('lailab_theme', next);
        });
    }

    // ---- Navbar Scroll ----
    const navbar = document.getElementById('navbar');
    let lastScroll = 0;

    window.addEventListener('scroll', () => {
        const scrollY = window.scrollY;
        if (scrollY > 50) {
            navbar.classList.add('scrolled');
        } else {
            navbar.classList.remove('scrolled');
        }
        lastScroll = scrollY;
    });

    // ---- Hamburger Menu ----
    const hamburger = document.getElementById('navHamburger');
    const navLinks = document.getElementById('navLinks');

    if (hamburger && navLinks) {
        hamburger.addEventListener('click', () => {
            navLinks.classList.toggle('open');
            hamburger.classList.toggle('active');
        });

        navLinks.querySelectorAll('.nav-link').forEach(link => {
            link.addEventListener('click', () => {
                navLinks.classList.remove('open');
                hamburger.classList.remove('active');
            });
        });
    }

    // ---- Animate Numbers ----
    const animateCounters = () => {
        document.querySelectorAll('[data-count]').forEach(el => {
            const target = parseInt(el.getAttribute('data-count'));
            const duration = 2000;
            const start = performance.now();

            const animate = (now) => {
                const elapsed = now - start;
                const progress = Math.min(elapsed / duration, 1);
                const eased = 1 - Math.pow(1 - progress, 3);
                el.textContent = Math.round(eased * target);

                if (progress < 1) {
                    requestAnimationFrame(animate);
                }
            };

            requestAnimationFrame(animate);
        });
    };

    // ---- Intersection Observer for Animations ----
    const observerOptions = {
        threshold: 0.1,
        rootMargin: '0px 0px -50px 0px'
    };

    const observer = new IntersectionObserver((entries) => {
        entries.forEach(entry => {
            if (entry.isIntersecting) {
                entry.target.classList.add('visible');

                // Animate counters  in hero stats
                if (entry.target.classList.contains('hero')) {
                    animateCounters();
                }
            }
        });
    }, observerOptions);

    // Observe sections
    document.querySelectorAll('.section, .hero').forEach(section => {
        observer.observe(section);
    });

    // ---- Smooth scroll for anchor links ----
    document.querySelectorAll('a[href^="#"]').forEach(anchor => {
        anchor.addEventListener('click', (e) => {
            e.preventDefault();
            const target = document.querySelector(anchor.getAttribute('href'));
            if (target) {
                const offset = 80;
                const top = target.getBoundingClientRect().top + window.scrollY - offset;
                window.scrollTo({ top, behavior: 'smooth' });
            }
        });
    });

    // ---- Parallax effect for hero glows ----
    let ticking = false;
    window.addEventListener('mousemove', (e) => {
        if (!ticking) {
            requestAnimationFrame(() => {
                const x = (e.clientX / window.innerWidth - 0.5) * 20;
                const y = (e.clientY / window.innerHeight - 0.5) * 20;

                const glow1 = document.querySelector('.hero-glow-1');
                const glow2 = document.querySelector('.hero-glow-2');
                if (glow1) glow1.style.transform = `translate(${x}px, ${y}px)`;
                if (glow2) glow2.style.transform = `translate(${-x}px, ${-y}px)`;

                ticking = false;
            });
            ticking = true;
        }
    });

    // ---- Bounding box animation ----
    const demoObjects = document.querySelectorAll('.demo-object');
    demoObjects.forEach((obj, i) => {
        const startDelay = i * 0.5;
        obj.style.animationDelay = `${startDelay}s`;
        obj.classList.add('demo-animate');
    });
});
