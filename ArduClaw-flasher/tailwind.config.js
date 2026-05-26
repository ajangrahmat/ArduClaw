/** @type {import('tailwindcss').Config} */
module.exports = {
  content: ['./*.html'],
  safelist: [
    // dynamic terminal classes (flog, wterm, lterm, anyterm)
    'text-[#484f58]', 'text-[#3fb950]', 'text-[#f85149]',
    'text-[#58a6ff]', 'text-[#d29922]', 'text-[#F9A61A]',
    // dynamic toast classes
    'bg-[#e6f7e6]', 'text-[#2d8a2d]', 'border-[#b7dfb8]',
    'bg-[#fcebeb]', 'text-[#a32d2d]', 'border-[#f09595]',
    'bg-[#fff8f0]', 'text-[#7a4a00]', 'border-[#fac775]',
    // toast animation
    'animate-[fadeUp_.3s_ease_both]',
  ],
  theme: {
    extend: {
      colors: { redpd: '#F31D1C', orpd: '#F9A61A', hitam: '#171717', abu: '#CFCFCF' },
      fontFamily: { nunito: ['Nunito', 'system-ui', 'sans-serif'] },
      borderRadius: { xl: '0.75rem', '2xl': '1rem', '3xl': '1.375rem' }
    }
  },
  plugins: [],
}
